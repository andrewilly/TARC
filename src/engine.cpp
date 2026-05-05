#include "engine.h"
#include "io.h"
#include "ui.h"
#include "types.h"
#include <cstring>
#include <map>
#include <filesystem>
#include <vector>
#include <iostream>
#include <chrono>
#include <future>
#include <algorithm>
#include <set>
#include <atomic>
#include <functional>
#include <fstream>
#include <deque>

#ifdef _WIN32
    #define NOMINMAX
    #include <windows.h>
#endif

#include "lzma.h"
extern "C" {
    #include "xxhash.h"
}
#include <zstd.h>
#include <lz4.h>
#include <lz4hc.h>
#include <brotli/encode.h>
#include <brotli/decode.h>

namespace fs = std::filesystem;

namespace {
    ProgressCallback* g_progress_callback = nullptr;
    std::atomic<bool> g_cancelled{false};
    Engine::CompressionStats g_stats;
}

void Engine::set_progress_callback(ProgressCallback* callback) {
    g_progress_callback = callback;
}

Engine::CompressionStats Engine::get_stats() {
    return g_stats;
}

void Engine::reset_stats() {
    g_stats = {};
    g_cancelled = false;
}

namespace CodecSelector {
    static const std::set<std::string> skip = { ".zip", ".7z", ".rar", ".gz", ".bz2", ".xz", ".lz", ".7zip", ".strk" };
    
    bool is_compressible(const std::string& ext) {
        std::string e = ext;
        std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return skip.find(e) == skip.end();
    }
    
    Codec select(const std::string& path, size_t size) {
        std::string ext = fs::path(path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        
        if (!is_compressible(ext)) return Codec::STORE;
        
        // PDF e documenti: ZSTD gestisce meglio i flussi gia compressi
        // (PDF contiene stream zlib/deflate interni che LZMA non comprime bene)
        if (ext == ".pdf" || ext == ".xps" || ext == ".oxps" ||
            ext == ".epub" || ext == ".mobi") {
            return Codec::ZSTD;
        }
        
        // File di testo e codice sorgente: LZMA2 con dizionario grande
        if (ext == ".txt" || ext == ".cpp" || ext == ".h" || ext == ".hpp" ||
            ext == ".c" || ext == ".py" || ext == ".js" || ext == ".ts" ||
            ext == ".json" || ext == ".xml" || ext == ".html" || ext == ".css" ||
            ext == ".sql" || ext == ".md" || ext == ".yaml" || ext == ".yml" ||
            ext == ".log" || ext == ".csv" || ext == ".ini" || ext == ".cfg") {
            return Codec::LZMA;
        }
        
        // Database: ZSTD con dizionario grande
        if (ext == ".mdb" || ext == ".accdb" || ext == ".mde" || ext == ".accde" ||
            ext == ".db" || ext == ".sqlite" || ext == ".sqlite3") {
            return Codec::ZSTD;
        }
        
        // Immagini gia compresse: STORE (non riduce)
        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".gif" ||
            ext == ".bmp" || ext == ".ico" || ext == ".webp" ||
            ext == ".mp3" || ext == ".mp4" || ext == ".avi" || ext == ".mkv" ||
            ext == ".wav" || ext == ".flac" || ext == ".ogg") {
            return Codec::STORE;
        }
        
        // Office (ZIP-based): LZMA per solid blocks
        if (ext == ".docx" || ext == ".xlsx" || ext == ".pptx" || ext == ".odt") {
            return Codec::LZMA;
        }
        
        // File piccoli: LZ4 veloce
        if (size < 64 * 1024) {
            return Codec::LZ4;
        }
        
        // Default: LZMA2 (miglior compressione per dati sconosciuti)
        return Codec::LZMA;
    }
}

static void report_progress(size_t current, size_t total, const std::string& current_file) {
    if (g_progress_callback) {
        g_progress_callback->on_progress(current, total, current_file);
    }
}

static void report_warning(const std::string& msg) {
    if (g_progress_callback) {
        g_progress_callback->on_warning(msg);
    }
}

static bool check_cancelled() {
    if (g_cancelled) return true;
    if (g_progress_callback && g_progress_callback->is_cancelled()) {
        g_cancelled = true;
        return true;
    }
    return false;
}

namespace Engine {

std::string normalize_path(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    // BUG FIX #7: rimuovi prefisso ./ iniziale
    while (path.size() >= 2 && path[0] == '.' && path[1] == '/') {
        path = path.substr(2);
    }
    return path;
}

struct ChunkResult {
    std::vector<char> compressed_data;
    uint32_t raw_size;
    Codec codec;
    bool success;
    std::string error_message;
};

// ============================================================================
// LZMA2 Codec — UPGRADE: LZMA2 + dizionario scalabile fino a 1GB
// Prima si usava lzma_easy_buffer_encode (solo LZMA1, max ~64MB dict).
// Ora si usa lzma_stream_buffer_encode con LZMA_FILTER_LZMA2 per:
//   - Miglior compressione su blocchi solid (come 7-Zip)
//   - Dizionario fino a 1GB ai livelli alti (vs 64MB)
//   - LZMA2 gestisce meglio i dati misti (testo + binario)
// ============================================================================

// Mappa livello CLI → dimensione dizionario LZMA2
static uint32_t lzma2_dict_size(int level) {
    if (level <= 1)  return   4 * 1024 * 1024; //   4 MB
    if (level <= 2)  return   8 * 1024 * 1024; //   8 MB
    if (level <= 3)  return  16 * 1024 * 1024; //  16 MB
    if (level <= 4)  return  32 * 1024 * 1024; //  32 MB
    if (level <= 6)  return  64 * 1024 * 1024; //  64 MB
    if (level <= 9)  return 128 * 1024 * 1024; // 128 MB
    if (level <= 12) return 256 * 1024 * 1024; // 256 MB
    if (level <= 15) return 512 * 1024 * 1024; // 512 MB
    return 1024UL * 1024 * 1024;              //   1 GB (level 16-19)
}

ChunkResult compress_lzma_optimal(const std::vector<char>& raw_data, int level) {
    ChunkResult res;
    res.raw_size = static_cast<uint32_t>(raw_data.size());
    res.codec = Codec::LZMA;
    res.success = false;
    
    if (raw_data.empty()) {
        res.compressed_data = raw_data;
        res.success = true;
        return res;
    }
    
    size_t max_out = lzma_stream_buffer_bound(raw_data.size());
    res.compressed_data.resize(max_out);
    size_t out_pos = 0;
    
    // Configura opzioni LZMA2 con dizionario scalabile
    lzma_options_lzma opt;
    uint32_t preset = static_cast<uint32_t>(std::min(level, 9));
    if (level >= 7) {
        preset |= LZMA_PRESET_EXTREME;
    }
    
    if (lzma_lzma_preset(&opt, preset) != LZMA_OK) {
        // Fallback se preset fallisce
        res.compressed_data = raw_data;
        res.codec = Codec::STORE;
        res.success = true;
        return res;
    }
    
    // Sovrascrivi il dizionario con la dimensione calcolata dal livello
    // (preset 9 = 64MB, ma noi vogliamo fino a 1GB ai livelli alti)
    uint32_t custom_dict = lzma2_dict_size(level);
    if (custom_dict > opt.dict_size) {
        opt.dict_size = custom_dict;
    }
    
    // Usa LZMA2 filter (piu efficiente di LZMA1 per blocchi solid)
    lzma_filter filters[2] = {};
    filters[0].id = LZMA_FILTER_LZMA2;
    filters[0].options = &opt;
    filters[1].id = UINT64_MAX;  // terminator (equivalente a LZMA_VLI_END, cross-platform)
    
    lzma_ret ret = lzma_stream_buffer_encode(
        filters,
        LZMA_CHECK_CRC64,
        nullptr,
        reinterpret_cast<const uint8_t*>(raw_data.data()),
        raw_data.size(),
        reinterpret_cast<uint8_t*>(res.compressed_data.data()),
        &out_pos,
        max_out
    );
    
    if (ret == LZMA_OK) {
        res.compressed_data.resize(out_pos);
        res.success = true;
    } else {
        res.compressed_data = raw_data;
        res.codec = Codec::STORE;
        res.success = true;
    }
    return res;
}

bool decompress_lzma(const std::vector<char>& compressed, std::vector<char>& decompressed) {
    if (decompressed.empty()) {
        decompressed.resize(1024 * 1024);
    }
    size_t src_pos = 0, dst_pos = 0;
    uint64_t limit = UINT64_MAX;
    
    lzma_ret ret = lzma_stream_buffer_decode(
        &limit, 0, nullptr,
        reinterpret_cast<const uint8_t*>(compressed.data()),
        &src_pos, compressed.size(),
        reinterpret_cast<uint8_t*>(decompressed.data()),
        &dst_pos, decompressed.size()
    );
    
    if (ret == LZMA_OK || ret == LZMA_STREAM_END) {
        decompressed.resize(dst_pos);
        return true;
    }
    return false;
}

// ============================================================================
// ZSTD Codec — UPGRADE: API avanzata con window log grande
// Prima si usava ZSTD_compress() semplice (max efficiente per default).
// Ora si usa ZSTD_CCtx + parametri avanzati:
//   - ZSTD_c_windowLog grande per long-range matching su blocchi solid
//   - ZSTD_c_strategy ultra a livelli alti (ZSTD_btultra2)
//   - Supporto completo livelli 1-19
// ============================================================================

ChunkResult compress_zstd(const std::vector<char>& raw_data, int level) {
    ChunkResult res;
    res.raw_size = static_cast<uint32_t>(raw_data.size());
    res.codec = Codec::ZSTD;
    res.success = false;
    
    if (raw_data.empty()) {
        res.compressed_data = raw_data;
        res.success = true;
        return res;
    }
    
    size_t bound = ZSTD_compressBound(raw_data.size());
    res.compressed_data.resize(bound);
    
    int zstd_level = std::clamp(level, 1, 19);
    
    // Crea contesto avanzato per ottimizzare la compressione
    ZSTD_CCtx* cctx = ZSTD_createCCtx();
    if (!cctx) {
        // Fallback: API semplice
        size_t comp_size = ZSTD_compress(
            res.compressed_data.data(), bound,
            raw_data.data(), raw_data.size(),
            zstd_level
        );
        if (ZSTD_isError(comp_size)) {
            res.compressed_data = raw_data;
            res.codec = Codec::STORE;
        } else {
            res.compressed_data.resize(comp_size);
            res.success = true;
        }
        return res;
    }
    
    // Window log grande per long-range matching su blocchi solid
    // Standard: log2(8MB) = 23. A livelli alti usiamo fino a 30 (1GB window)
    int window_log = 23; // 8MB default
    if (zstd_level >= 10) window_log = 27;      // 128MB
    if (zstd_level >= 14) window_log = 29;      // 512MB
    if (zstd_level >= 17) window_log = 30;      // 1GB
    
    // Assicura che il window log non ecceda la dimensione dei dati
    size_t data_bits = 0;
    size_t tmp = raw_data.size();
    while (tmp > 0) { data_bits++; tmp >>= 1; }
    if (window_log > 0 && static_cast<int>(data_bits) < window_log) {
        window_log = static_cast<int>(data_bits);
    }
    // ZSTD richiede window_log >= 10
    if (window_log < 10) window_log = 10;
    
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, zstd_level);
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_windowLog, window_log);
    
    // Usa strategia ultra ai livelli piu alti (miglior compressione)
    if (zstd_level >= 16) {
        ZSTD_CCtx_setParameter(cctx, ZSTD_c_strategy, ZSTD_btultra2);
    } else if (zstd_level >= 10) {
        ZSTD_CCtx_setParameter(cctx, ZSTD_c_strategy, ZSTD_btultra);
    }
    
    // Abilita checksum a livelli alti per integrita
    if (zstd_level >= 10) {
        ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag, 1);
    }
    
    size_t comp_size = ZSTD_compress2(
        cctx,
        res.compressed_data.data(), bound,
        raw_data.data(), raw_data.size()
    );
    
    ZSTD_freeCCtx(cctx);
    
    if (ZSTD_isError(comp_size)) {
        // Fallback: store uncompressed
        res.compressed_data = raw_data;
        res.codec = Codec::STORE;
        res.success = true;
        return res;
    }
    
    res.compressed_data.resize(comp_size);
    res.success = true;
    return res;
}

bool decompress_zstd(const std::vector<char>& compressed, std::vector<char>& decompressed) {
    unsigned long long frame_size = ZSTD_getFrameContentSize(
        compressed.data(), compressed.size()
    );
    
    if (frame_size != ZSTD_CONTENTSIZE_UNKNOWN &&
        frame_size != ZSTD_CONTENTSIZE_ERROR &&
        frame_size > 0) {
        // Content size is known from frame header
        decompressed.resize(static_cast<size_t>(frame_size));
        size_t result = ZSTD_decompress(
            decompressed.data(), decompressed.size(),
            compressed.data(), compressed.size()
        );
        if (ZSTD_isError(result)) return false;
        decompressed.resize(result);
        return true;
    }
    
    // Content size unknown: decompress with growing buffer
    size_t buf_size = compressed.size() * 4;
    if (buf_size < 65536) buf_size = 65536;
    
    for (int attempt = 0; attempt < 8; ++attempt) {
        decompressed.resize(buf_size);
        size_t result = ZSTD_decompress(
            decompressed.data(), buf_size,
            compressed.data(), compressed.size()
        );
        if (!ZSTD_isError(result)) {
            decompressed.resize(result);
            return true;
        }
        buf_size *= 2;
    }
    return false;
}

// ============================================================================
// LZ4 Codec — FEATURE #1: implementazione nativa
// ============================================================================

ChunkResult compress_lz4(const std::vector<char>& raw_data, int level) {
    ChunkResult res;
    res.raw_size = static_cast<uint32_t>(raw_data.size());
    res.codec = Codec::LZ4;
    res.success = false;
    
    if (raw_data.empty()) {
        res.compressed_data = raw_data;
        res.success = true;
        return res;
    }
    
    int src_size = static_cast<int>(raw_data.size());
    int max_out = LZ4_compressBound(src_size);
    if (max_out <= 0) {
        res.compressed_data = raw_data;
        res.codec = Codec::STORE;
        res.success = true;
        return res;
    }
    
    res.compressed_data.resize(static_cast<size_t>(max_out));
    
    int comp_size;
    if (level >= 4) {
        // LZ4HC per livelli alti (miglior compressione, piu lento)
        int hc_level = std::clamp(level, 4, 12);
        comp_size = LZ4_compress_HC(
            raw_data.data(), res.compressed_data.data(),
            src_size, max_out, hc_level
        );
    } else {
        // LZ4 default veloce
        comp_size = LZ4_compress_default(
            raw_data.data(), res.compressed_data.data(),
            src_size, max_out
        );
    }
    
    if (comp_size <= 0 || static_cast<size_t>(comp_size) >= raw_data.size()) {
        // Compressione fallita o non riduce: fallback STORE
        res.compressed_data = raw_data;
        res.codec = Codec::STORE;
        res.success = true;
        return res;
    }
    
    res.compressed_data.resize(static_cast<size_t>(comp_size));
    res.success = true;
    return res;
}

bool decompress_lz4(const std::vector<char>& compressed, std::vector<char>& decompressed) {
    int src_size = static_cast<int>(compressed.size());
    int dst_capacity = static_cast<int>(decompressed.size());
    
    if (dst_capacity == 0) {
        // Dimensione target sconosciuta: usare buffer generoso
        dst_capacity = std::max(src_size * 4, 65536);
        decompressed.resize(static_cast<size_t>(dst_capacity));
    }
    
    int dec_size = LZ4_decompress_safe(
        compressed.data(), decompressed.data(),
        src_size, dst_capacity
    );
    
    if (dec_size < 0) {
        // Buffer troppo piccolo: ritentare con dimensione doppia
        if (dst_capacity > 0 && static_cast<size_t>(dst_capacity) == decompressed.size()) {
            dst_capacity *= 2;
            decompressed.resize(static_cast<size_t>(dst_capacity));
            dec_size = LZ4_decompress_safe(
                compressed.data(), decompressed.data(),
                src_size, dst_capacity
            );
        }
        if (dec_size < 0) return false;
    }
    
    decompressed.resize(static_cast<size_t>(dec_size));
    return true;
}

// ============================================================================
// Brotli Codec — UPGRADE: window size scalabile con il livello
// Prima: lgwin fisso a 22 (4MB) per tutti i livelli.
// Ora: lgwin scala da 20 (1MB) a 26 (64MB) in base al livello,
// permettendo long-range matching su blocchi solid grandi.
// ============================================================================

ChunkResult compress_brotli(const std::vector<char>& raw_data, int level) {
    ChunkResult res;
    res.raw_size = static_cast<uint32_t>(raw_data.size());
    res.codec = Codec::BR;
    res.success = false;
    
    if (raw_data.empty()) {
        res.compressed_data = raw_data;
        res.success = true;
        return res;
    }
    
    // Brotli bound: input_size + input_size/8 + 1024 (raccomandazione ufficiale)
    size_t max_out = raw_data.size() + raw_data.size() / 8 + 1024;
    res.compressed_data.resize(max_out);
    
    int quality = std::clamp(level, 0, 11);
    
    // Window size scalabile: da 1MB (livello 1) a 64MB (livello 11+)
    // Livelli alti beneficiano di window piu grande per long-range matching
    int lgwin = 20; // 1MB default
    if (quality >= 3)  lgwin = 22; //   4 MB
    if (quality >= 5)  lgwin = 23; //   8 MB
    if (quality >= 7)  lgwin = 24; //  16 MB
    if (quality >= 9)  lgwin = 25; //  32 MB
    if (quality >= 11) lgwin = 26; //  64 MB
    
    // Assicura che il window non ecceda la dimensione dei dati
    size_t data_bits = 0;
    size_t tmp = raw_data.size();
    while (tmp > 0) { data_bits++; tmp >>= 1; }
    if (static_cast<int>(data_bits) < lgwin) {
        lgwin = static_cast<int>(data_bits);
    }
    if (lgwin < 10) lgwin = 10; // Brotli minimum
    
    size_t encoded_size = max_out;
    BROTLI_BOOL result = BrotliEncoderCompress(
        quality, lgwin, BROTLI_MODE_GENERIC,
        raw_data.size(),
        reinterpret_cast<const uint8_t*>(raw_data.data()),
        &encoded_size,
        reinterpret_cast<uint8_t*>(res.compressed_data.data())
    );
    
    if (!result) {
        // Fallback: store uncompressed
        res.compressed_data = raw_data;
        res.codec = Codec::STORE;
        res.success = true;
        return res;
    }
    
    // Se la compressione non riduce la dimensione, fallback STORE
    if (encoded_size >= raw_data.size()) {
        res.compressed_data = raw_data;
        res.codec = Codec::STORE;
        res.success = true;
        return res;
    }
    
    res.compressed_data.resize(encoded_size);
    res.success = true;
    return res;
}

bool decompress_brotli(const std::vector<char>& compressed, std::vector<char>& decompressed) {
    size_t decoded_size = decompressed.size();
    
    if (decoded_size == 0) {
        // Dimensione target sconosciuta: stimare
        decoded_size = compressed.size() * 4;
        if (decoded_size < 65536) decoded_size = 65536;
        decompressed.resize(decoded_size);
    }
    
    BrotliDecoderResult result = BrotliDecoderDecompress(
        compressed.size(),
        reinterpret_cast<const uint8_t*>(compressed.data()),
        &decoded_size,
        reinterpret_cast<uint8_t*>(decompressed.data())
    );
    
    if (result == BROTLI_DECODER_RESULT_SUCCESS) {
        decompressed.resize(decoded_size);
        return true;
    }
    
    if (result == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT) {
        // Buffer troppo piccolo: ritentare con dimensione doppia
        for (int attempt = 0; attempt < 4; ++attempt) {
            decoded_size = decompressed.size() * 2;
            decompressed.resize(decoded_size);
            result = BrotliDecoderDecompress(
                compressed.size(),
                reinterpret_cast<const uint8_t*>(compressed.data()),
                &decoded_size,
                reinterpret_cast<uint8_t*>(decompressed.data())
            );
            if (result == BROTLI_DECODER_RESULT_SUCCESS) {
                decompressed.resize(decoded_size);
                return true;
            }
        }
    }
    
    return false;
}

// ============================================================================
// compress_worker — FEATURE #1: dispatch al codec selezionato (non piu' solo LZMA)
// ============================================================================

ChunkResult compress_worker(std::vector<char> raw_data, int level, Codec chosen_codec) {
    ChunkResult res;
    res.raw_size = static_cast<uint32_t>(raw_data.size());
    res.codec = chosen_codec;
    res.success = false;

    if (raw_data.empty()) {
        res.compressed_data = std::move(raw_data);
        res.success = true;
        return res;
    }

    if (raw_data.size() < 4096) {
        res.compressed_data = std::move(raw_data);
        res.codec = Codec::STORE;
        res.success = true;
        return res;
    }

    if (chosen_codec == Codec::STORE) {
        res.compressed_data = std::move(raw_data);
        res.success = true;
        return res;
    }

    uint32_t saved_raw_size = res.raw_size;
    
    switch (chosen_codec) {
        case Codec::ZSTD:
            res = compress_zstd(raw_data, level);
            break;
        case Codec::LZMA:
            res = compress_lzma_optimal(raw_data, level);
            break;
        case Codec::LZ4:
            res = compress_lz4(raw_data, level);
            break;
        case Codec::BR:
            res = compress_brotli(raw_data, level);
            break;
        default:
            // Fallback sicuro: LZMA
            res = compress_lzma_optimal(raw_data, level);
            break;
    }
    
    res.raw_size = saved_raw_size; // preserva il raw_size corretto
    return res;
}

// ============================================================================
// decompress_chunk — FEATURE #1: dispatch al decompressore corretto
// ============================================================================

bool decompress_chunk(const std::vector<char>& compressed, std::vector<char>& decompressed, Codec codec) {
    if (codec == Codec::STORE) {
        decompressed = compressed;
        return true;
    }
    
    switch (codec) {
        case Codec::LZMA:
            return decompress_lzma(compressed, decompressed);
        case Codec::ZSTD:
            return decompress_zstd(compressed, decompressed);
        case Codec::LZ4:
            return decompress_lz4(compressed, decompressed);
        case Codec::BR:
            return decompress_brotli(compressed, decompressed);
        default:
            return decompress_lzma(compressed, decompressed);
    }
}

// ============================================================================
// SFX — Self-Extracting Archive (integrato in tarc.exe)
// ============================================================================
// Layout del file SFX generato:
//   [tarc.exe][Archivio TARC .strk][SfxTrailer (24 byte)]
//
// Quando l'utente lancia il file .exe generato, tarc.exe rileva il trailer
// SFX alla fine del file e auto-estrae l'archivio embeddato.
// Non serve nessun stub separato — tarc.exe e' sia il compressore che lo stub.
// ============================================================================

// ============================================================================
// Legge il trailer SFX dagli ultimi 24 byte di un file
// ============================================================================
static bool read_sfx_trailer(const std::string& exe_path, SfxTrailer& trailer) {
    std::ifstream f(exe_path, std::ios::binary);
    if (!f) return false;

    f.seekg(0, std::ios::end);
    auto file_size = f.tellg();
    if (file_size < static_cast<std::streamoff>(SFX_TRAILER_SIZE)) return false;

    f.seekg(-static_cast<std::streamoff>(SFX_TRAILER_SIZE), std::ios::end);
    f.read(reinterpret_cast<char*>(&trailer), SFX_TRAILER_SIZE);
    if (f.gcount() != SFX_TRAILER_SIZE) return false;

    if (std::memcmp(trailer.magic, SFX_MAGIC, 8) != 0) return false;

    // Validazione coerenza offset/dimensione
    uint64_t fsize = static_cast<uint64_t>(file_size);
    if (trailer.archive_offset >= fsize) return false;
    if (trailer.archive_offset + trailer.archive_size > fsize - SFX_TRAILER_SIZE) return false;

    return true;
}

bool is_sfx_mode(const std::string& exe_path) {
    SfxTrailer trailer;
    return read_sfx_trailer(exe_path, trailer);
}

TarcResult extract_sfx(const std::string& exe_path,
                       const std::string& output_dir,
                       bool overwrite) {
    TarcResult res;
    res.ok = false;

    // Leggi il trailer
    SfxTrailer trailer;
    if (!read_sfx_trailer(exe_path, trailer)) {
        res.error = TarcError::CorruptedArchive;
        res.message = "Not a valid TARC SFX archive.";
        return res;
    }

    // Estrai l'archivio TARC embeddato in un file temporaneo
    fs::path temp_dir = fs::temp_directory_path();
    fs::path temp_archive = temp_dir / "tarc_sfx_temp.strk";

    {
        std::ifstream self(exe_path, std::ios::binary);
        if (!self) {
            res.error = TarcError::AccessDenied;
            res.message = "Cannot open: " + exe_path;
            return res;
        }

        std::ofstream out(temp_archive, std::ios::binary);
        if (!out) {
            res.error = TarcError::AccessDenied;
            res.message = "Cannot create temporary file.";
            return res;
        }

        self.clear();
        self.seekg(static_cast<std::streamoff>(trailer.archive_offset));

        const size_t BUF_SIZE = 1024 * 1024;
        std::vector<char> buf(BUF_SIZE);
        uint64_t remaining = trailer.archive_size;

        while (remaining > 0) {
            size_t to_read = static_cast<size_t>(std::min(remaining, static_cast<uint64_t>(BUF_SIZE)));
            self.read(buf.data(), to_read);
            if (self.gcount() != static_cast<std::streamsize>(to_read)) {
                out.close();
                std::error_code ec;
                fs::remove(temp_archive, ec);
                res.error = TarcError::CorruptedArchive;
                res.message = "Failed to read embedded archive.";
                return res;
            }
            out.write(buf.data(), to_read);
            remaining -= to_read;
        }
        out.flush();
        out.close();

        if (!out.good()) {
            std::error_code ec;
            fs::remove(temp_archive, ec);
            res.error = TarcError::WriteFailed;
            res.message = "Failed to write temporary archive.";
            return res;
        }
    }

    // Estrai usando il motore TARC
    ExtractOptions xopts;
    xopts.test_only = false;
    xopts.verify = true;
    xopts.overwrite = overwrite;
    if (!output_dir.empty()) {
        xopts.output_dir = output_dir;
    }

    res = extract(temp_archive.string(), {}, xopts);

    // Cleanup temporaneo
    std::error_code ec;
    fs::remove(temp_archive, ec);

    return res;
}

TarcResult create_sfx(const std::string& archive_path, const std::string& sfx_name) {
    TarcResult res;
    res.ok = false;

    // Usa l'eseguibile corrente (tarc.exe) come stub
    std::string stub_path = IO::get_self_path();
    if (stub_path.empty()) {
        res.error = TarcError::FileNotFound;
        res.message = "Cannot determine current executable path.";
        return res;
    }

    // Verifica che l'archivio esista
    if (!fs::exists(archive_path)) {
        res.error = TarcError::FileNotFound;
        res.message = "Archive not found: " + archive_path;
        return res;
    }

    // Determina le dimensioni dei file
    uint64_t stub_size = static_cast<uint64_t>(fs::file_size(stub_path));
    uint64_t archive_size = static_cast<uint64_t>(fs::file_size(archive_path));

    if (stub_size == 0) {
        res.error = TarcError::CorruptedArchive;
        res.message = "SFX stub is empty.";
        return res;
    }
    if (archive_size == 0) {
        res.error = TarcError::CorruptedArchive;
        res.message = "Archive is empty.";
        return res;
    }

    // L'archivio TARC inizia subito dopo lo stub
    uint64_t archive_offset = stub_size;

    // Costruisci il trailer
    SfxTrailer trailer;
    std::memcpy(trailer.magic, SFX_MAGIC, 8);
    trailer.archive_offset = archive_offset;
    trailer.archive_size = archive_size;

    // Apri tutti i file
    std::ifstream stub_in(stub_path, std::ios::binary);
    std::ifstream archive_in(archive_path, std::ios::binary);
    std::ofstream sfx_out(sfx_name, std::ios::binary);

    if (!stub_in || !archive_in || !sfx_out) {
        res.error = TarcError::AccessDenied;
        res.message = "Failed to open files for SFX creation.";
        return res;
    }

    // Scrivi lo stub EXE
    const size_t COPY_BUF = 1024 * 1024;
    std::vector<char> buf(COPY_BUF);

    // 1) Copia stub
    uint64_t remaining = stub_size;
    while (remaining > 0) {
        size_t to_read = static_cast<size_t>(std::min(remaining, static_cast<uint64_t>(COPY_BUF)));
        stub_in.read(buf.data(), to_read);
        if (!stub_in) {
            res.error = TarcError::CorruptedArchive;
            res.message = "Failed to read stub.";
            return res;
        }
        sfx_out.write(buf.data(), to_read);
        remaining -= to_read;
    }
    stub_in.close();

    // 2) Copia archivio TARC
    remaining = archive_size;
    while (remaining > 0) {
        size_t to_read = static_cast<size_t>(std::min(remaining, static_cast<uint64_t>(COPY_BUF)));
        archive_in.read(buf.data(), to_read);
        if (!archive_in) {
            res.error = TarcError::CorruptedArchive;
            res.message = "Failed to read archive.";
            return res;
        }
        sfx_out.write(buf.data(), to_read);
        remaining -= to_read;
    }
    archive_in.close();

    // 3) Scrivi il trailer (ultimi 24 byte)
    sfx_out.write(reinterpret_cast<const char*>(&trailer), SFX_TRAILER_SIZE);
    sfx_out.flush();

    if (!sfx_out.good()) {
        res.error = TarcError::WriteFailed;
        res.message = "Failed to write SFX file.";
        return res;
    }
    sfx_out.close();

    // Calcola dimensione finale per il report
    uint64_t sfx_total = stub_size + archive_size + SFX_TRAILER_SIZE;
    res.ok = true;
    res.bytes_in = archive_size;
    res.bytes_out = sfx_total;
    res.message = "SFX archive created: " + sfx_name
                + " (" + std::to_string(sfx_total / (1024 * 1024)) + " MB)";
    return res;
}

// ============================================================================
// ARCH-003: Helper per scrivere un chunk con checksum xxHash e error handling
// ============================================================================

static bool write_chunk(FILE* f, Codec codec, uint32_t raw_size,
                        const std::vector<char>& data, uint64_t& bytes_out) {
    uint64_t cksum = 0;
    if (!data.empty()) {
        cksum = XXH64(data.data(), data.size(), 0);
    }

    ChunkHeader ch = {
        static_cast<uint32_t>(codec),
        raw_size,
        static_cast<uint32_t>(data.size()),
        cksum
    };

    if (fwrite(&ch, sizeof(ch), 1, f) != 1) return false;
    if (!data.empty() && fwrite(data.data(), 1, data.size(), f) != data.size()) return false;
    bytes_out += data.size();
    return true;
}

// ============================================================================
// STREAMING COMPRESSION — Anti-OOM per file grandi
// ============================================================================
// Legge il file sorgente a blocchi di 256KB e comprime usando API streaming.
// Memory usage costante: ~128-256MB (indipendente dalla dimensione del file).
// Il decompressore esistente funziona senza modifiche (formati compatibili).
// ============================================================================

constexpr size_t STREAM_BUF_SIZE = 256 * 1024;  // 256KB I/O buffers

// Helper: flush del solid buffer (usato dallo streaming path)
static bool flush_solid_buffer(FILE* f, std::vector<char>& solid_buf, bool& solid_has_files,
                                size_t solid_toc_begin, Codec solid_codec, int level,
                                std::vector<FileEntry>& final_toc, uint64_t& bytes_out) {
    if (!solid_has_files || solid_buf.empty()) return true;
    ChunkResult solid_cr = compress_worker(std::move(solid_buf), level, solid_codec);
    if (!solid_cr.success) return false;
    uint64_t solid_offset = static_cast<uint64_t>(IO::tarc_ftell(f));
    if (!write_chunk(f, solid_cr.codec, solid_cr.raw_size, solid_cr.compressed_data, bytes_out))
        return false;
    Codec actual_codec = solid_cr.codec;
    for (size_t j = solid_toc_begin; j < final_toc.size(); ++j) {
        final_toc[j].meta.offset = solid_offset;
        final_toc[j].meta.codec = static_cast<uint8_t>(actual_codec);
    }
    solid_buf.clear();
    solid_buf.reserve(128 * 1024 * 1024);
    solid_has_files = false;
    return true;
}

// Streaming LZMA2 — memory costante (~128MB encoder)
static bool stream_compress_lzma2(FILE* src_f, FILE* dst_f, int level,
                                   uint64_t& out_comp_size, uint64_t& out_checksum) {
    lzma_options_lzma opt;
    uint32_t preset = static_cast<uint32_t>(std::min(level, 9));
    if (level >= 7) preset |= LZMA_PRESET_EXTREME;
    if (lzma_lzma_preset(&opt, preset) != LZMA_OK) return false;

    // Cap dict a 64MB per streaming (encoder usa ~2x dict)
    if (opt.dict_size > 64u * 1024 * 1024) opt.dict_size = 64u * 1024 * 1024;

    lzma_filter filters[2] = {};
    filters[0].id = LZMA_FILTER_LZMA2;
    filters[0].options = &opt;
    filters[1].id = UINT64_MAX;

    lzma_stream strm = LZMA_STREAM_INIT;
    if (lzma_stream_encoder(&strm, filters, LZMA_CHECK_CRC64) != LZMA_OK) return false;

    std::vector<uint8_t> in_buf(STREAM_BUF_SIZE);
    std::vector<uint8_t> out_buf(STREAM_BUF_SIZE * 4);
    out_comp_size = 0;
    out_checksum = 0;
    lzma_action action = LZMA_RUN;

    XXH64_state_t* xxh = XXH64_createState();
    XXH64_reset(xxh, 0);
    bool ok = true;

    while (true) {
        if (strm.avail_in == 0 && action == LZMA_RUN) {
            size_t n = fread(in_buf.data(), 1, in_buf.size(), src_f);
            if (n == 0) { action = LZMA_FINISH; }
            else { strm.next_in = in_buf.data(); strm.avail_in = n; }
        }
        strm.next_out = out_buf.data();
        strm.avail_out = out_buf.size();
        lzma_ret ret = lzma_code(&strm, action);
        if (ret != LZMA_OK && ret != LZMA_STREAM_END) { ok = false; break; }
        size_t have = out_buf.size() - strm.avail_out;
        if (have > 0) {
            XXH64_update(xxh, out_buf.data(), have);
            if (fwrite(out_buf.data(), 1, have, dst_f) != have) { ok = false; break; }
            out_comp_size += have;
        }
        if (ret == LZMA_STREAM_END) break;
    }

    out_checksum = XXH64_digest(xxh);
    lzma_end(&strm);
    XXH64_freeState(xxh);
    return ok;
}

// Streaming ZSTD — memory costante
static bool stream_compress_zstd(FILE* src_f, FILE* dst_f, int level,
                                  uint64_t& out_comp_size, uint64_t& out_checksum) {
    ZSTD_CStream* cs = ZSTD_createCStream();
    if (!cs) return false;
    int zl = std::clamp(level, 1, 19);
    ZSTD_CCtx_setParameter(cs, ZSTD_c_compressionLevel, zl);
    int wlog = 23; // 8MB
    if (zl >= 10) wlog = 25; // 32MB
    if (zl >= 16) wlog = 27; // 128MB
    ZSTD_CCtx_setParameter(cs, ZSTD_c_windowLog, wlog);
    if (zl >= 16) ZSTD_CCtx_setParameter(cs, ZSTD_c_strategy, ZSTD_btultra2);
    else if (zl >= 10) ZSTD_CCtx_setParameter(cs, ZSTD_c_strategy, ZSTD_btultra);

    std::vector<uint8_t> in_buf(STREAM_BUF_SIZE);
    std::vector<uint8_t> out_buf(STREAM_BUF_SIZE * 4);
    out_comp_size = 0;
    out_checksum = 0;
    XXH64_state_t* xxh = XXH64_createState();
    XXH64_reset(xxh, 0);
    bool ok = true;

    while (true) {
        size_t n = fread(in_buf.data(), 1, in_buf.size(), src_f);
        bool last = (n < in_buf.size());
        ZSTD_inBuffer input = {in_buf.data(), n, 0};
        ZSTD_outBuffer output = {out_buf.data(), out_buf.size(), 0};

        size_t ret = ZSTD_compressStream2(cs, &output, &input,
                                           last ? ZSTD_e_end : ZSTD_e_continue);
        if (ZSTD_isError(ret)) { ok = false; break; }

        if (output.pos > 0) {
            XXH64_update(xxh, out_buf.data(), output.pos);
            if (fwrite(out_buf.data(), 1, output.pos, dst_f) != output.pos) { ok = false; break; }
            out_comp_size += output.pos;
        }
        if (last && ret == 0) break;
    }

    out_checksum = XXH64_digest(xxh);
    ZSTD_freeCStream(cs);
    XXH64_freeState(xxh);
    return ok;
}

// Streaming Brotli — memory costante
static bool stream_compress_brotli(FILE* src_f, FILE* dst_f, int level,
                                    uint64_t& out_comp_size, uint64_t& out_checksum) {
    BrotliEncoderState* bs = BrotliEncoderCreateInstance(nullptr, nullptr, nullptr);
    if (!bs) return false;
    int quality = std::clamp(level, 0, 11);
    BrotliEncoderSetParameter(bs, BROTLI_PARAM_QUALITY, quality);
    BrotliEncoderSetParameter(bs, BROTLI_PARAM_LGWIN, 24); // 16MB

    std::vector<uint8_t> in_buf(STREAM_BUF_SIZE);
    std::vector<uint8_t> out_buf(STREAM_BUF_SIZE * 4);
    out_comp_size = 0;
    out_checksum = 0;
    XXH64_state_t* xxh = XXH64_createState();
    XXH64_reset(xxh, 0);
    bool ok = true;

    while (true) {
        size_t avail_in = fread(in_buf.data(), 1, in_buf.size(), src_f);
        bool last = (avail_in < in_buf.size());
        BrotliEncoderOperation op = last ? BROTLI_OPERATION_FINISH : BROTLI_OPERATION_PROCESS;
        const uint8_t* next_in = in_buf.data();

        while (true) {
            size_t avail_out = out_buf.size();
            uint8_t* next_out = out_buf.data();
            BROTLI_BOOL r = BrotliEncoderCompressStream(bs, op, &avail_in, &next_in, &avail_out, &next_out, nullptr);
            size_t have = out_buf.size() - avail_out;
            if (have > 0) {
                XXH64_update(xxh, out_buf.data(), have);
                if (fwrite(out_buf.data(), 1, have, dst_f) != have) { ok = false; goto done_brotli; }
                out_comp_size += have;
            }
            if (!r) { ok = false; goto done_brotli; }
            if (op == BROTLI_OPERATION_FINISH && BrotliEncoderIsFinished(bs)) goto done_brotli;
            if (avail_in > 0 && op == BROTLI_OPERATION_PROCESS) continue;
            break;
        }
        if (last) break;
    }
done_brotli:
    out_checksum = XXH64_digest(xxh);
    BrotliEncoderDestroyInstance(bs);
    XXH64_freeState(xxh);
    return ok;
}

// Scrive un chunk usando compressione streaming (seek-back per ChunkHeader)
static bool write_chunk_streaming(FILE* archive_f, const std::string& source_path,
                                   uintmax_t source_size, int level, Codec codec,
                                   uint64_t& bytes_out) {
    int64_t hdr_pos = IO::tarc_ftell(archive_f);
    if (hdr_pos == -1) return false;

    ChunkHeader placeholder = {0, 0, 0, 0};
    if (fwrite(&placeholder, sizeof(placeholder), 1, archive_f) != 1) return false;

    FILE* src_f = fopen(source_path.c_str(), "rb");
    if (!src_f) return false;

    // LZ4 non ha streaming buono → fallback ZSTD
    Codec actual = codec;
    if (codec == Codec::LZ4) actual = Codec::ZSTD;

    uint64_t csz = 0, cksum = 0;
    bool ok = false;
    switch (actual) {
        case Codec::LZMA: ok = stream_compress_lzma2(src_f, archive_f, level, csz, cksum); break;
        case Codec::ZSTD: ok = stream_compress_zstd(src_f, archive_f, level, csz, cksum); break;
        case Codec::BR:   ok = stream_compress_brotli(src_f, archive_f, level, csz, cksum); break;
        default:          ok = stream_compress_zstd(src_f, archive_f, level, csz, cksum); actual = Codec::ZSTD; break;
    }
    fclose(src_f);
    if (!ok) return false;

    int64_t end_pos = IO::tarc_ftell(archive_f);
    if (end_pos == -1) return false;
    if (IO::tarc_fseek(archive_f, hdr_pos, SEEK_SET) != 0) return false;

    ChunkHeader hdr = {
        static_cast<uint32_t>(actual),
        static_cast<uint32_t>(source_size > UINT32_MAX ? 0 : source_size),
        static_cast<uint32_t>(csz > UINT32_MAX ? 0 : csz),
        cksum
    };
    if (fwrite(&hdr, sizeof(hdr), 1, archive_f) != 1) return false;
    if (IO::tarc_fseek(archive_f, end_pos, SEEK_SET) != 0) return false;

    bytes_out += csz;
    return true;
}

// ============================================================================
// Struttura per tracciare i file appartenenti a un chunk solid
// ============================================================================
struct SolidChunkFiles {
    size_t toc_begin; // primo indice in final_toc
    size_t toc_end;   // ultimo indice in final_toc
};

// ============================================================================
// COMPRESS — con Feature #1 (codec nativi), Feature #5 (Entry.offset)
// ============================================================================

TarcResult compress(const std::string& arch_path, const std::vector<std::string>& inputs, CompressOptions opts) {
    TarcResult res;
    res.ok = false;
    reset_stats();
    
    int level = opts.level;
    bool has_codec_override = (opts.codec != Codec::LZMA);  // LZMA = auto/default
    
    std::vector<std::string> expanded_files;
    for (const auto& in : inputs) {
        IO::expand_path(in, expanded_files);
    }
    if (expanded_files.empty()) {
        res.error = TarcError::FileNotFound;
        res.message = "No files found.";
        return res;
    }

    std::vector<FileEntry> final_toc;
    std::map<uint64_t, uint32_t> hash_map;
    Header h{};
    
    std::memcpy(h.magic, TARC_MAGIC, 4);
    h.version = TARC_VERSION;

    FILE* f = fopen(arch_path.c_str(), "wb");
    if (!f) {
        res.error = TarcError::AccessDenied;
        res.message = "Cannot write archive.";
        return res;
    }

    if (fwrite(&h, sizeof(h), 1, f) != 1) {
        fclose(f);
        res.error = TarcError::WriteFailed;
        res.message = "Failed to write header.";
        return res;
    }

    // FEATURE #5: traccia l'offset dei chunk nell'archivio
    uint64_t data_offset = sizeof(Header);

    // Anti-OOM: solid buffer ridotto da 1GB a 128MB
    constexpr size_t CHUNK_THRESHOLD = 128 * 1024 * 1024;
    // File piu grandi di questo vengono compressi in streaming (non caricati in RAM)
    constexpr size_t MAX_IN_MEMORY = 256 * 1024 * 1024;
    std::vector<char> solid_buf;
    solid_buf.reserve(CHUNK_THRESHOLD);
    
    std::future<ChunkResult> future_chunk;
    bool worker_active = false;
    
    // FEATURE #5: traccia quali toc index appartengono al chunk pending (solid async)
    std::deque<SolidChunkFiles> pending_solid_ranges;
    
    // FEATURE #5: traccia il primo toc index della generazione solid corrente
    size_t solid_toc_begin = 0;
    bool solid_has_files = false;
    // Codec per il buffer solid corrente (il primo file del chunk decide il codec)
    Codec solid_codec = Codec::LZMA;

    auto write_pending_chunk = [&](std::future<ChunkResult>& fut) -> bool {
        if (check_cancelled()) return false;
        
        ChunkResult cr = fut.get();
        if (!cr.success) return false;
        
        // FEATURE #5: registra l'offset del chunk prima di scriverlo
        uint64_t chunk_offset = static_cast<uint64_t>(ftell(f));
        
        if (!write_chunk(f, cr.codec, cr.raw_size, cr.compressed_data, res.bytes_out))
            return false;
        
        // FEATURE #5: aggiorna Entry.offset per tutti i file in questo chunk solid
        if (!pending_solid_ranges.empty()) {
            SolidChunkFiles range = pending_solid_ranges.front();
            pending_solid_ranges.pop_front();
            for (size_t j = range.toc_begin; j <= range.toc_end; ++j) {
                final_toc[j].meta.offset = chunk_offset;
            }
        }
        
        return true;
    };

    auto start_time = TarcUtil::safe_now();
    
    for (size_t i = 0; i < expanded_files.size(); ++i) {
        if (check_cancelled()) {
            res.error = TarcError::Cancelled;
            res.message = "Cancelled.";
            fclose(f);
            return res;
        }
        
        const std::string& disk_path = expanded_files[i];
        report_progress(i + 1, expanded_files.size(), fs::path(disk_path).filename().string());

        if (!fs::exists(disk_path)) {
            report_warning("File not found: " + disk_path);
            continue;
        }
        
        uintmax_t fsize = fs::file_size(disk_path);
        
        if (fsize > static_cast<uintmax_t>(SIZE_MAX)) {
            report_warning("File too large (overflow): " + disk_path);
            continue;
        }
        
        // ============================================================
        // Anti-OOM: file grandi → streaming (non caricati in RAM)
        // ============================================================
        constexpr size_t MAX_IN_MEMORY = 256 * 1024 * 1024;
        bool use_streaming = (fsize > MAX_IN_MEMORY);
        
        std::vector<char> data;
        if (!use_streaming) {
            try {
                data.resize(static_cast<size_t>(fsize));
            } catch (const std::bad_alloc&) {
                use_streaming = true; // fallback a streaming se OOM
            }
        }

        bool read_ok = false;
        uint64_t h64 = 0;
        XXH64_state_t* const state = XXH64_createState();
        if (state) XXH64_reset(state, 0);
        
        FILE* in_f = fopen(disk_path.c_str(), "rb");
        if (in_f) {
            if (use_streaming) {
                // Streaming: calcola hash leggendo a blocchi (senza caricare tutto)
                std::vector<char> hbuf(64 * 1024);
                size_t n;
                while ((n = fread(hbuf.data(), 1, hbuf.size(), in_f)) > 0) {
                    if (state) XXH64_update(state, hbuf.data(), n);
                }
                read_ok = true;
            } else {
                size_t read_res = fread(data.data(), 1, static_cast<size_t>(fsize), in_f);
                if (read_res == static_cast<size_t>(fsize)) {
                    read_ok = true;
                    if (state) XXH64_update(state, data.data(), static_cast<size_t>(fsize));
                }
            }
            fclose(in_f);
        } else {
            report_warning("Cannot open file: " + disk_path);
        }

        if (state) {
            h64 = XXH64_digest(state);
            XXH64_freeState(state);
        }

        if (!read_ok) {
            report_warning("Failed to read file: " + disk_path);
            continue;
        }

        FileEntry fe;
        fe.name = normalize_path(disk_path);
        fe.meta.orig_size = fsize;
        fe.meta.xxhash = h64;

        constexpr size_t STORE_THRESHOLD = 2048;

        // FEATURE #1 / #6: il codec nel TOC riflette il codec REALMENTE usato
        Codec selected_codec;
        if (has_codec_override) {
            selected_codec = opts.codec;  // override CLI: --zstd, --lz4, etc.
        } else {
            selected_codec = CodecSelector::select(disk_path, fsize);  // auto
        }

        fe.meta.timestamp = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                fs::last_write_time(disk_path).time_since_epoch()
            ).count()
        );

        if (hash_map.count(h64)) {
            fe.meta.is_duplicate = 1;
            fe.meta.duplicate_of_idx = hash_map[h64];
            fe.meta.codec = static_cast<uint8_t>(Codec::STORE);
            fe.meta.offset = 0; // duplicati non hanno dati
            g_stats.duplicates_skipped++;
        } else {
            hash_map[h64] = static_cast<uint32_t>(final_toc.size());
            fe.meta.is_duplicate = 0;

            // ============================================================
            // STREAMING: file grandi compressi direttamente da disco
            // Non carica il file in RAM, usa ~128-256MB costanti
            // ============================================================
            if (use_streaming && selected_codec != Codec::STORE) {
                // Flush solid buffer pendente prima dello streaming
                if (worker_active && !write_pending_chunk(future_chunk)) {
                    res.error = TarcError::CompressionFailed;
                    res.message = "Chunk compression failed.";
                    fclose(f);
                    return res;
                }
                worker_active = false;

                if (!flush_solid_buffer(f, solid_buf, solid_has_files,
                                        solid_toc_begin, solid_codec, level,
                                        final_toc, res.bytes_out)) {
                    res.error = TarcError::CompressionFailed;
                    res.message = "Solid flush failed.";
                    fclose(f);
                    return res;
                }

                // Stream-comprime il file grande direttamente
                uint64_t stream_offset = static_cast<uint64_t>(IO::tarc_ftell(f));
                Codec stream_codec = selected_codec;

                if (!write_chunk_streaming(f, disk_path, fsize, level, stream_codec, res.bytes_out)) {
                    res.error = TarcError::CompressionFailed;
                    res.message = "Streaming compression failed: " + disk_path;
                    fclose(f);
                    return res;
                }

                // Aggiorna TOC
                fe.meta.offset = stream_offset;
                if (stream_codec == Codec::LZ4) stream_codec = Codec::ZSTD; // fallback
                fe.meta.codec = static_cast<uint8_t>(stream_codec);
                g_stats.bytes_read += fsize;
                data_offset = static_cast<uint64_t>(IO::tarc_ftell(f));
            } else if (fsize <= STORE_THRESHOLD) {
                // ============================================================
                // BUG FIX: flush pending solid buffer BEFORE writing STORE chunk
                // Senza questo, i chunk solid vengono scritti DOPO i chunk STORE,
                // causando ordinamento errato sul disco e fallimento dell'estrazione.
                // ============================================================
                if (solid_has_files && !solid_buf.empty()) {
                    // Attendiamo eventuale compressione async pending
                    if (worker_active && !write_pending_chunk(future_chunk)) {
                        res.error = TarcError::CompressionFailed;
                        res.message = "Chunk compression failed.";
                        fclose(f);
                        return res;
                    }
                    worker_active = false;

                    // Comprimi e scrivi il solid buffer accumulato
                    ChunkResult solid_cr = compress_worker(std::move(solid_buf), level, solid_codec);
                    if (!solid_cr.success) {
                        res.error = TarcError::CompressionFailed;
                        res.message = "Solid chunk compression failed.";
                        fclose(f);
                        return res;
                    }

                    uint64_t solid_offset = static_cast<uint64_t>(ftell(f));
                    if (!write_chunk(f, solid_cr.codec, solid_cr.raw_size, solid_cr.compressed_data, res.bytes_out)) {
                        res.error = TarcError::WriteFailed;
                        res.message = "Failed to write solid chunk.";
                        fclose(f);
                        return res;
                    }

                    // Aggiorna Entry.offset per tutti i file in questo gruppo solid
                    for (size_t j = solid_toc_begin; j < final_toc.size(); ++j) {
                        final_toc[j].meta.offset = solid_offset;
                    }
                    // Aggiorna il codec nel TOC al codec REALMENTE usato
                    // (compress_worker puo' cambiare codec a STORE per buffer < 4096)
                    Codec actual_codec = solid_cr.codec;
                    for (size_t j = solid_toc_begin; j < final_toc.size(); ++j) {
                        final_toc[j].meta.codec = static_cast<uint8_t>(actual_codec);
                    }

                    solid_buf.clear();
                    solid_buf.reserve(CHUNK_THRESHOLD);
                    solid_has_files = false;
                    // Sincronizza data_offset con la posizione reale sul file
                    data_offset = static_cast<uint64_t>(ftell(f));
                }

                // File STORE: scritti direttamente, NON nel solid_buf
                ChunkResult cr;
                cr.compressed_data = data;
                cr.raw_size = static_cast<uint32_t>(fsize);
                cr.codec = Codec::STORE;
                cr.success = true;

                // FEATURE #5: offset del chunk STORE
                fe.meta.offset = data_offset;
                fe.meta.codec = static_cast<uint8_t>(Codec::STORE);

                if (!write_chunk(f, cr.codec, cr.raw_size, cr.compressed_data, res.bytes_out)) {
                    res.error = TarcError::WriteFailed;
                    res.message = "Failed to write STORE chunk.";
                    fclose(f);
                    return res;
                }
                data_offset += sizeof(ChunkHeader) + data.size();
                g_stats.bytes_read += fsize;
                solid_has_files = false; // resetta tracciamento solid
            } else if (solid_buf.size() + fsize > CHUNK_THRESHOLD && !solid_buf.empty()) {
                // Solid buffer pieno: scarica il blocco corrente
                
                // Scrivi il chunk pending precedente (se async compress in corso)
                if (worker_active && !write_pending_chunk(future_chunk)) {
                    res.error = TarcError::CompressionFailed;
                    res.message = "Chunk compression failed.";
                    fclose(f);
                    return res;
                }
                worker_active = false;
                
                // Registra i toc index per questo chunk solid che sta per essere compresso
                pending_solid_ranges.push_back({solid_toc_begin, final_toc.size() - 1});
                
                // Avvia compressione async per il buffer solid corrente
                future_chunk = std::async(
                    std::launch::async,
                    compress_worker,
                    std::move(solid_buf),
                    level,
                    solid_codec  // FEATURE #1: usa il codec del buffer solid
                );
                worker_active = true;
                solid_buf.clear();
                solid_buf.reserve(CHUNK_THRESHOLD);
                
                // Inizia nuova generazione solid con il file corrente
                solid_buf.insert(solid_buf.end(), data.begin(), data.end());
                solid_toc_begin = final_toc.size(); // questo file sara' a questo index
                solid_codec = selected_codec;       // il codec del nuovo chunk solid
                solid_has_files = true;
                g_stats.bytes_read += fsize;
            } else {
                // File va nel buffer solid corrente
                solid_buf.insert(solid_buf.end(), data.begin(), data.end());
                g_stats.bytes_read += fsize;
                
                if (!solid_has_files) {
                    // Primo file del chunk solid: registra inizio e codec
                    solid_toc_begin = final_toc.size();
                    solid_codec = selected_codec;
                    solid_has_files = true;
                }
            }
            
            // FEATURE #1: imposta il codec nel TOC al codec realmente usato
            if (fsize > STORE_THRESHOLD) {
                fe.meta.codec = static_cast<uint8_t>(solid_codec);
            }
        }
        
        final_toc.push_back(fe);
        g_stats.files_processed++;
    }

    // Scrivi l'ultimo chunk pending (se compressione async in corso)
    if (worker_active && !write_pending_chunk(future_chunk)) {
        res.error = TarcError::CompressionFailed;
        res.message = "Final chunk failed.";
        fclose(f);
        return res;
    }
    worker_active = false;
    
    // Scrive il buffer solid finale (se non vuoto)
    if (!solid_buf.empty()) {
        ChunkResult last = compress_worker(std::move(solid_buf), level, solid_codec);
        
        // FEATURE #5: offset del chunk finale
        uint64_t last_chunk_offset = static_cast<uint64_t>(ftell(f));
        
        if (!write_chunk(f, last.codec, last.raw_size, last.compressed_data, res.bytes_out)) {
            res.error = TarcError::WriteFailed;
            res.message = "Failed to write final chunk.";
            fclose(f);
            return res;
        }
        
        // FEATURE #5: aggiorna Entry.offset per i file dell'ultimo chunk solid
        for (size_t j = solid_toc_begin; j < final_toc.size(); ++j) {
            final_toc[j].meta.offset = last_chunk_offset;
        }
    }

    ChunkHeader end_mark = {0, 0, 0, 0};
    if (fwrite(&end_mark, sizeof(end_mark), 1, f) != 1) {
        res.error = TarcError::WriteFailed;
        res.message = "Failed to write end marker.";
        fclose(f);
        return res;
    }
    IO::write_toc(f, h, final_toc);
    fflush(f);
    fclose(f);
    
    g_stats.bytes_in = g_stats.bytes_read;
    g_stats.bytes_out = res.bytes_out;
    g_stats.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        TarcUtil::safe_now() - start_time
    );
    
    res.ok = true;
    res.bytes_in = g_stats.bytes_read;
    res.bytes_out = res.bytes_out;
    res.message = "Compression completed.";
    return res;
}

// ============================================================================
// Pattern matching
// ============================================================================

static bool match_pattern_impl(const std::string& target, const std::string& pattern, size_t ti, size_t pi) {
    while (ti < target.size() && pi < pattern.size()) {
        if (pattern[pi] == '*') {
            while (pi < pattern.size() && pattern[pi] == '*') pi++;
            if (pi == pattern.size()) return true;
            for (size_t k = ti; k <= target.size(); ++k) {
                if (match_pattern_impl(target, pattern, k, pi)) return true;
            }
            return false;
        } else if (pattern[pi] == '?') {
            ti++;
            pi++;
        } else {
            if (target[ti] != pattern[pi]) return false;
            ti++;
            pi++;
        }
    }
    while (pi < pattern.size() && pattern[pi] == '*') pi++;
    return ti == target.size() && pi == pattern.size();
}

static bool match_pattern(const std::string& full_path, const std::string& pattern) {
    if (pattern.empty()) return true;

    std::string target = full_path;
    if (pattern.find('/') == std::string::npos && pattern.find('\\') == std::string::npos) {
        target = fs::path(full_path).filename().string();
    }

    bool has_wildcard = (pattern.find('*') != std::string::npos || pattern.find('?') != std::string::npos);
    if (!has_wildcard) {
        return target == pattern;
    }

    if (pattern.find("**/") == 0) {
        std::string sub_pattern = pattern.substr(3);
        return match_pattern_impl(full_path, sub_pattern, 0, 0);
    }

    return match_pattern_impl(target, pattern, 0, 0);
}

// ============================================================================
// ARCH-001: Helper per leggere il prossimo chunk decompresso
// ============================================================================

static TarcError read_next_block(FILE* f, std::vector<char>& block, size_t& block_pos) {
    if (block_pos >= block.size()) {
        ChunkHeader ch;
        if (fread(&ch, sizeof(ch), 1, f) != 1 || ch.raw_size == 0) {
            return TarcError::CorruptedArchive;
        }

        if (ch.comp_size > TARC_MAX_CHUNK_SIZE || ch.raw_size > TARC_MAX_CHUNK_SIZE) {
            return TarcError::CorruptedArchive;
        }

        std::vector<char> comp(ch.comp_size);
        if (fread(comp.data(), 1, ch.comp_size, f) != ch.comp_size) {
            return TarcError::CorruptedArchive;
        }

        block.resize(ch.raw_size);
        Codec codec = static_cast<Codec>(ch.codec);
        if (!decompress_chunk(comp, block, codec)) {
            return TarcError::DecompressionFailed;
        }
        block_pos = 0;
    }
    return TarcError::None;
}

// ============================================================================
// EXTRACT — Feature #4: supporto output_dir + ExtractOptions
// ============================================================================

TarcResult extract(const std::string& arch_path, const std::vector<std::string>& patterns,
                   ExtractOptions opts) {
    TarcResult res;
    res.ok = false;
    reset_stats();

    FILE* f = fopen(arch_path.c_str(), "rb");
    if (!f) {
        res.error = TarcError::FileNotFound;
        res.message = "Archive not found.";
        return res;
    }

    Header h;
    if (fread(&h, sizeof(h), 1, f) != 1) {
        fclose(f);
        res.error = TarcError::InvalidHeader;
        res.message = "Invalid header.";
        return res;
    }

    if (!IO::validate_archive_header(h)) {
        fclose(f);
        if (std::memcmp(h.magic, TARC_MAGIC, 4) != 0) {
            res.error = TarcError::InvalidHeader;
            res.message = "Not a valid TARC archive (bad magic).";
        } else {
            res.error = TarcError::UnsupportedVersion;
            res.message = "Unsupported archive version: " + std::to_string(h.version);
        }
        return res;
    }

    std::vector<FileEntry> toc;
    if (!IO::read_toc(f, h, toc)) {
        fclose(f);
        res.error = TarcError::CorruptedArchive;
        res.message = "Cannot read TOC.";
        return res;
    }
    
    // Seek to start of chunk data (right after header)
    IO::tarc_fseek(f, static_cast<int64_t>(sizeof(Header)), SEEK_SET);
    
    std::vector<char> current_block;
    size_t block_pos = 0;
    std::map<std::string, int> flat_names_counter;

    // FEATURE #4: crea la directory di output se specificata
    if (!opts.output_dir.empty()) {
        fs::path out_dir(opts.output_dir);
        if (out_dir.is_relative()) {
            out_dir = fs::current_path() / out_dir;
        }
        std::error_code ec;
        fs::create_directories(out_dir, ec);
        if (ec) {
            fclose(f);
            res.error = TarcError::AccessDenied;
            res.message = "Cannot create output directory: " + opts.output_dir;
            return res;
        }
    }

    for (size_t i = 0; i < toc.size(); ++i) {
        if (check_cancelled()) {
            res.error = TarcError::Cancelled;
            res.message = "Cancelled.";
            fclose(f);
            return res;
        }
        
        auto& fe = toc[i];
        report_progress(i + 1, toc.size(), fe.name);
        
        bool should_extract = patterns.empty();
        if (!should_extract) {
            for (const auto& pat : patterns) {
                if (match_pattern(fe.name, pat)) {
                    should_extract = true;
                    break;
                }
            }
        }

        if (!should_extract) {
            if (fe.meta.is_duplicate) continue;
            TarcError err = read_next_block(f, current_block, block_pos);
            if (err == TarcError::CorruptedArchive) break;
            if (err != TarcError::None) {
                fclose(f);
                res.error = err;
                res.message = "Chunk read failed.";
                return res;
            }
            block_pos += fe.meta.orig_size;
            continue;
        }

        if (fe.meta.is_duplicate) continue;

        {
            TarcError err = read_next_block(f, current_block, block_pos);
            if (err == TarcError::CorruptedArchive) break;
            if (err != TarcError::None) {
                fclose(f);
                res.error = err;
                res.message = "Chunk read failed.";
                return res;
            }
        }

        std::string final_path = fe.name;
        if (opts.flat_mode) {
            fs::path p(fe.name);
            std::string filename = p.filename().string();
            
            if (flat_names_counter.count(filename)) {
                flat_names_counter[filename]++;
                size_t dot_pos = filename.find_last_of('.');
                if (dot_pos != std::string::npos) {
                    filename = filename.substr(0, dot_pos) + "_" + 
                              std::to_string(flat_names_counter[filename]) + 
                              filename.substr(dot_pos);
                } else {
                    filename += "_" + std::to_string(flat_names_counter[filename]);
                }
            } else {
                flat_names_counter[filename] = 0;
            }
            final_path = filename;
        }

        if (!opts.test_only) {
            std::string safe_path = IO::sanitize_extract_path(final_path);
            if (safe_path.empty()) {
                report_warning("Path traversal blocked: " + fe.name);
                fclose(f);
                res.error = TarcError::PathTraversal;
                res.message = "Unsafe path in archive: " + fe.name;
                return res;
            }

            // FEATURE #4: preponi output_dir se specificato
            std::string full_output_path = safe_path;
            if (!opts.output_dir.empty()) {
                std::string dir = opts.output_dir;
                std::replace(dir.begin(), dir.end(), '\\', '/');
                if (!dir.empty() && dir.back() != '/') {
                    dir += '/';
                }
                full_output_path = dir + safe_path;
            }

            if (!IO::write_file_to_disk(full_output_path,
                                   current_block.data() + block_pos,
                                   static_cast<size_t>(fe.meta.orig_size),
                                   fe.meta.timestamp, opts.overwrite)) {
                if (IO::file_exists(full_output_path)) {
                    res.error = TarcError::AccessDenied;
                    res.message = "File exists (use --force): " + full_output_path;
                } else {
                    res.error = TarcError::AccessDenied;
                    res.message = "Failed to write: " + full_output_path;
                }
                fclose(f);
                return res;
            }

            // SEC-006: Verifica integrita' xxHash del file estratto
            if (opts.verify && fe.meta.xxhash != 0) {
                XXH64_state_t* const vstate = XXH64_createState();
                if (vstate) {
                    XXH64_reset(vstate, 0);
                    XXH64_update(vstate, current_block.data() + block_pos,
                                 static_cast<size_t>(fe.meta.orig_size));
                    uint64_t extracted_hash = XXH64_digest(vstate);
                    XXH64_freeState(vstate);

                    if (extracted_hash != fe.meta.xxhash) {
                        report_warning("XXH64 mismatch: " + fe.name);
                        fclose(f);
                        res.error = TarcError::IntegrityCheckFailed;
                        res.message = "Checksum mismatch: " + fe.name;
                        return res;
                    }
                }
            }
        } else {
            // SEC-006: Verifica integrita' in modalita' test
            if (opts.verify && fe.meta.xxhash != 0) {
                XXH64_state_t* const vstate = XXH64_createState();
                if (vstate) {
                    XXH64_reset(vstate, 0);
                    XXH64_update(vstate, current_block.data() + block_pos,
                                 static_cast<size_t>(fe.meta.orig_size));
                    uint64_t extracted_hash = XXH64_digest(vstate);
                    XXH64_freeState(vstate);

                    if (extracted_hash != fe.meta.xxhash) {
                        fclose(f);
                        res.error = TarcError::IntegrityCheckFailed;
                        res.message = "Integrity check failed: " + fe.name;
                        return res;
                    }
                }
            }
        }
        
        res.bytes_out += fe.meta.orig_size;
        block_pos += fe.meta.orig_size;
        g_stats.files_processed++;
    }
    
    fclose(f);
    res.ok = true;
    res.message = opts.test_only ? "Test completed." : "Extraction completed.";
    return res;
}

// ============================================================================
// LIST
// ============================================================================

TarcResult list(const std::string& arch_path, size_t offset) {
    TarcResult res;
    res.ok = false;
    
    FILE* f = fopen(arch_path.c_str(), "rb");
    if (!f) {
        res.error = TarcError::FileNotFound;
        res.message = "Archive not found.";
        return res;
    }
    
    if (offset > 0) {
        IO::tarc_fseek(f, static_cast<int64_t>(offset), SEEK_SET);
    }
    
    Header h;
    if (fread(&h, sizeof(h), 1, f) != 1) {
        fclose(f);
        res.error = TarcError::InvalidHeader;
        res.message = "Invalid header.";
        return res;
    }

    if (!IO::validate_archive_header(h)) {
        fclose(f);
        if (std::memcmp(h.magic, TARC_MAGIC, 4) != 0) {
            res.error = TarcError::InvalidHeader;
            res.message = "Not a valid TARC archive (bad magic).";
        } else {
            res.error = TarcError::UnsupportedVersion;
            res.message = "Unsupported archive version: " + std::to_string(h.version);
        }
        return res;
    }

    std::vector<FileEntry> toc;
    h.toc_offset += offset;
    if (!IO::read_toc(f, h, toc)) {
        fclose(f);
        res.error = TarcError::CorruptedArchive;
        res.message = "Cannot read TOC.";
        return res;
    }
    
    for (const auto& fe : toc) {
        UI::print_list_entry(
            fe.name,
            fe.meta.orig_size,
            fe.meta.is_duplicate ? 0 : fe.meta.orig_size,
            static_cast<Codec>(fe.meta.codec)
        );
    }
    
    fclose(f);
    res.ok = true;
    res.message = "Listed " + std::to_string(toc.size()) + " files.";
    return res;
}

TarcResult remove_files(const std::string&, const std::vector<std::string>&) { 
    TarcResult res;
    res.ok = false;
    res.error = TarcError::Unknown;
    res.message = "Remove not supported.";
    return res;
}

} // namespace Engine
