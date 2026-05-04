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
        std::transform(e.begin(), e.end(), e.begin(), ::tolower);
        return skip.find(e) == skip.end();
    }
    
    Codec select(const std::string& path, size_t size) {
        std::string ext = fs::path(path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        
        if (!is_compressible(ext)) return Codec::STORE;
        
        if (ext == ".txt" || ext == ".cpp" || ext == ".h" || ext == ".hpp" ||
            ext == ".c" || ext == ".py" || ext == ".js" || ext == ".ts" ||
            ext == ".json" || ext == ".xml" || ext == ".html" || ext == ".css" ||
            ext == ".sql" || ext == ".md" || ext == ".yaml" || ext == ".yml" ||
            ext == ".log" || ext == ".csv" || ext == ".ini" || ext == ".cfg") {
            return Codec::LZMA;
        }
        
        if (ext == ".mdb" || ext == ".accdb" || ext == ".mde" || ext == ".accde" ||
            ext == ".db" || ext == ".sqlite" || ext == ".sqlite3") {
            return Codec::ZSTD;
        }
        
        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".gif" ||
            ext == ".bmp" || ext == ".ico" || ext == ".webp") {
            return Codec::LZMA;
        }
        
        if (ext == ".docx" || ext == ".xlsx" || ext == ".pptx" || ext == ".odt") {
            return Codec::LZMA;
        }
        
        if (size < 64 * 1024) {
            return Codec::LZ4;
        }
        
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
// LZMA Codec
// ============================================================================

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
    
    uint32_t preset = static_cast<uint32_t>(std::min(level, 9));
    if (level >= 7) {
        preset |= LZMA_PRESET_EXTREME;
    }
    
    lzma_ret ret = lzma_easy_buffer_encode(
        preset,
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
// ZSTD Codec — FEATURE #1: implementazione nativa
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
    size_t comp_size = ZSTD_compress(
        res.compressed_data.data(), bound,
        raw_data.data(), raw_data.size(),
        zstd_level
    );
    
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
// Brotli Codec — FEATURE #1: implementazione nativa (API corretta)
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
    int lgwin = 22; // window size massimo per miglior compressione
    
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
// SFX
// ============================================================================

TarcResult create_sfx(const std::string& archive_path, const std::string& sfx_name) {
    TarcResult res;
    res.ok = false;
    
    std::string stub_path = "tarc_sfx_stub.exe";
    if (!fs::exists(stub_path)) {
        res.error = TarcError::FileNotFound;
        res.message = "Stub SFX not found.";
        return res;
    }

    std::ifstream stub_in(stub_path, std::ios::binary);
    std::ifstream data_in(archive_path, std::ios::binary);
    std::ofstream sfx_out(sfx_name, std::ios::binary);

    if (!stub_in || !data_in || !sfx_out) {
        res.error = TarcError::AccessDenied;
        res.message = "Failed to create SFX.";
        return res;
    }

    sfx_out << stub_in.rdbuf();
    sfx_out << data_in.rdbuf();
    
    res.ok = true;
    res.message = "SFX created.";
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
// Struttura per tracciare i file appartenenti a un chunk solid
// Usata per popolare Entry.offset retroattivamente (FEATURE #5)
// ============================================================================
struct SolidChunkFiles {
    size_t toc_begin; // primo indice in final_toc
    size_t toc_end;   // ultimo indice in final_toc
};

// ============================================================================
// COMPRESS — con Feature #1 (codec nativi), Feature #5 (Entry.offset)
// ============================================================================

TarcResult compress(const std::string& arch_path, const std::vector<std::string>& inputs, int level) {
    TarcResult res;
    res.ok = false;
    reset_stats();
    
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

    constexpr size_t CHUNK_THRESHOLD = 1024 * 1024 * 1024;
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
        
        std::vector<char> data;
        try {
            data.resize(static_cast<size_t>(fsize));
        } catch (const std::bad_alloc&) {
            res.error = TarcError::OutOfMemory;
            res.message = "Out of memory: " + disk_path;
            report_warning(res.message);
            continue;
        }

        bool read_ok = false;
        uint64_t h64 = 0;
        XXH64_state_t* const state = XXH64_createState();
        if (state) XXH64_reset(state, 0);
        
        FILE* in_f = fopen(disk_path.c_str(), "rb");
        if (in_f) {
            size_t read_res = fread(data.data(), 1, static_cast<size_t>(fsize), in_f);
            if (read_res == static_cast<size_t>(fsize)) {
                read_ok = true;
                if (state) XXH64_update(state, data.data(), static_cast<size_t>(fsize));
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

        // FEATURE #1: il codec nel TOC riflette il codec REALMENTE usato
        Codec selected_codec = CodecSelector::select(disk_path, fsize);

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

            if (fsize <= STORE_THRESHOLD) {
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
