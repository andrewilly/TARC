#include "engine.h"
#include "io.h"
#include "ui.h"
#include <cstring>
#include <set>
#include <map>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <future>
#include <queue>

// ─── DIPENDENZE COMPRESSIONE ─────────────────────────────────────────────────
#include "zstd.h"
#include "lz4.h"
#include "lz4hc.h"
#include "lzma.h"
#include <brotli/encode.h>
#include <brotli/decode.h>

extern "C" {
    #include "xxhash.h"
}

namespace fs = std::filesystem;

namespace CodecSelector {
    static const std::set<std::string> COMPRESSED_EXTS = {
        ".zip", ".7z", ".rar", ".gz", ".bz2", ".xz", ".lz4", ".zst", ".br", ".tar",
        ".jpg", ".jpeg", ".avif", ".heic", ".mp4", ".mkv", ".avi", ".mov", ".webm", ".flv"
    };

    Codec select(const std::string& path, int level) {
        std::string ext = fs::path(path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (COMPRESSED_EXTS.count(ext)) return Codec::LZ4;
        // Se il livello è alto, forziamo i codec pesanti
        if (level >= 18) return Codec::BR; 
        if (level >= 10) return Codec::LZMA;
        return Codec::ZSTD;
    }
}

namespace Engine {

struct CompressedChunk {
    uint32_t raw_size;
    uint32_t comp_size;
    std::vector<char> data;
    bool is_compressed;
};

// Worker con parametri "Ultra"
static CompressedChunk worker_compress(Codec codec, std::vector<char> src, int level) {
    CompressedChunk res;
    res.raw_size = (uint32_t)src.size();
    res.data.resize(res.raw_size + 65536);
    size_t c_sz = 0;
    res.is_compressed = false;

    if (codec == Codec::ZSTD) {
        c_sz = ZSTD_compress(res.data.data(), res.data.size(), src.data(), src.size(), level);
        if (!ZSTD_isError(c_sz)) { res.comp_size = (uint32_t)c_sz; res.is_compressed = (c_sz < res.raw_size); }
    } 
    else if (codec == Codec::LZ4) {
        c_sz = (level > 3) ? LZ4_compress_HC(src.data(), res.data.data(), (int)src.size(), (int)res.data.size(), level)
                           : LZ4_compress_default(src.data(), res.data.data(), (int)src.size(), (int)res.data.size());
        if (c_sz > 0) { res.comp_size = (uint32_t)c_sz; res.is_compressed = (c_sz < (size_t)res.raw_size); }
    }
    else if (codec == Codec::LZMA) {
        // LZMA Extreme Mode (come 7zip -mx9)
        uint32_t preset = std::clamp(level, 0, 9) | LZMA_PRESET_EXTREME;
        size_t out_pos = 0;
        lzma_ret ret = lzma_easy_buffer_encode(preset, LZMA_CHECK_CRC64, NULL, (const uint8_t*)src.data(), src.size(), (uint8_t*)res.data.data(), &out_pos, res.data.size());
        if (ret == LZMA_OK) { res.comp_size = (uint32_t)out_pos; res.is_compressed = (out_pos < src.size()); }
    }
    else if (codec == Codec::BR) {
        // BROTLI ULTRA CONFIG
        size_t out_sz = res.data.size();
        int quality = std::clamp(level, 0, 11); // Max 11
        // Finestra di 24-bit (16MB) per trovare ridondanze distanti
        if (BrotliEncoderCompress(quality, 24, BROTLI_MODE_GENERIC, src.size(), (const uint8_t*)src.data(), &out_sz, (uint8_t*)res.data.data())) {
            res.comp_size = (uint32_t)out_sz; res.is_compressed = (out_sz < src.size());
        }
    }

    if (!res.is_compressed) { res.data = std::move(src); res.comp_size = res.raw_size; }
    else { res.data.resize(res.comp_size); }
    return res;
}

TarcResult compress(const std::string& archive_path, const std::vector<std::string>& files, bool append, int level) {
    TarcResult result;
    Header h; std::vector<FileEntry> toc;
    
    FILE* f = fopen(archive_path.c_str(), append && fs::exists(archive_path) ? "rb+" : "wb");
    if (!f) { result.ok = false; return result; }

    if (append && IO::read_toc(f, h, toc)) { fseek(f, (long)h.toc_offset, SEEK_SET); }
    else { memcpy(h.magic, TARC_MAGIC, 4); h.version = TARC_VERSION; IO::write_bytes(f, &h, sizeof(h)); }

    size_t max_threads = std::max(1u, std::thread::hardware_concurrency());
    std::queue<std::future<CompressedChunk>> pipeline;
    
    // Buffer Solid: accumula dati da più file prima di comprimere
    std::vector<char> solid_buffer;
    const size_t SOLID_BLOCK_SIZE = 16 * 1024 * 1024; // Blocchi da 16MB come 7zip

    for (const auto& path : files) {
        if (!fs::is_regular_file(path)) continue;
        
        FileEntry fe;
        fe.name = fs::relative(path).string();
        fe.meta.timestamp = std::chrono::duration_cast<std::chrono::seconds>(fs::last_write_time(path).time_since_epoch()).count();
        fe.meta.orig_size = fs::file_size(path);
        fe.meta.offset = ftell(f); // In modalità Solid, l'offset punta all'inizio del blocco comune

        FILE* in = fopen(path.c_str(), "rb");
        if (!in) continue;

        std::vector<char> file_data(fe.meta.orig_size);
        fread(file_data.data(), 1, fe.meta.orig_size, in);
        fclose(in);

        // Hash del singolo file
        XXH64_state_t* hs = XXH64_createState(); XXH64_reset(hs, 0);
        XXH64_update(hs, file_data.data(), file_data.size());
        fe.meta.xxhash = XXH64_digest(hs); XXH64_freeState(hs);

        // Aggiungi al buffer solido
        solid_buffer.insert(solid_buffer.end(), file_data.begin(), file_data.end());
        
        Codec current_codec = CodecSelector::select(path, level);
        fe.meta.codec = (uint8_t)current_codec;

        // Se il buffer solido è pieno, lancia la compressione
        if (solid_buffer.size() >= SOLID_BLOCK_SIZE) {
            while (pipeline.size() >= max_threads) {
                auto cc = pipeline.front().get(); pipeline.pop();
                ChunkHeader ch = { cc.raw_size, cc.comp_size };
                IO::write_bytes(f, &ch, sizeof(ch));
                IO::write_bytes(f, cc.data.data(), cc.comp_size);
                result.bytes_out += cc.comp_size;
            }
            pipeline.push(std::async(std::launch::async, worker_compress, current_codec, std::move(solid_buffer), level));
            solid_buffer.clear();
        }

        toc.push_back(fe);
        UI::print_add(fe.name, fe.meta.orig_size, (Codec)fe.meta.codec, 0.0f); // Ratio calcolato alla fine
        result.bytes_in += fe.meta.orig_size;
    }

    // Svuota buffer rimanente
    if (!solid_buffer.empty()) {
        pipeline.push(std::async(std::launch::async, worker_compress, Codec::BR, std::move(solid_buffer), level));
    }

    while (!pipeline.empty()) {
        auto cc = pipeline.front().get(); pipeline.pop();
        ChunkHeader ch = { cc.raw_size, cc.comp_size };
        IO::write_bytes(f, &ch, sizeof(ch));
        IO::write_bytes(f, cc.data.data(), cc.comp_size);
        result.bytes_out += cc.comp_size;
    }

    ChunkHeader end = {0, 0}; IO::write_bytes(f, &end, sizeof(end));
    IO::write_toc(f, h, toc); fclose(f);
    result.ok = true; return result;
}

// Nota: Extract deve essere aggiornato per gestire i blocchi solidi (lettura sequenziale)
TarcResult extract(const std::string& archive_path, bool test_only) {
    TarcResult result;
    FILE* f = fopen(archive_path.c_str(), "rb");
    Header h; std::vector<FileEntry> toc;
    if (!f || !IO::read_toc(f, h, toc)) { if(f) fclose(f); result.ok = false; return result; }
    
    ZSTD_DCtx* dctx = ZSTD_createDCtx();
    std::vector<char> c_buf, d_buf;

    // In modalità Solid, leggiamo l'archivio sequenzialmente e distribuiamo i byte ai file
    fseek(f, sizeof(Header), SEEK_SET);
    size_t current_file_idx = 0;
    std::vector<char> current_decompressed_block;
    size_t block_pos = 0;

    while (current_file_idx < toc.size()) {
        ChunkHeader ch;
        if (!IO::read_bytes(f, &ch, sizeof(ch)) || ch.raw_size == 0) break;

        c_buf.resize(ch.comp_size);
        d_buf.resize(ch.raw_size);
        IO::read_bytes(f, c_buf.data(), ch.comp_size);

        // Decompressione (Logica identica a prima)
        auto& fe_sample = toc[current_file_idx];
        if (fe_sample.meta.codec == (uint8_t)Codec::ZSTD) ZSTD_decompressDCtx(dctx, d_buf.data(), ch.raw_size, c_buf.data(), ch.comp_size);
        else if (fe_sample.meta.codec == (uint8_t)Codec::LZ4) LZ4_decompress_safe(c_buf.data(), d_buf.data(), ch.comp_size, ch.raw_size);
        else if (fe_sample.meta.codec == (uint8_t)Codec::LZMA) {
             uint64_t mem = 256*1024*1024; size_t ip=0, op=0;
             lzma_stream_buffer_decode(&mem, 0, NULL, (uint8_t*)c_buf.data(), &ip, ch.comp_size, (uint8_t*)d_buf.data(), &op, ch.raw_size);
        }
        else if (fe_sample.meta.codec == (uint8_t)Codec::BR) {
            size_t dsz = ch.raw_size;
            BrotliDecoderDecompress(ch.comp_size, (uint8_t*)c_buf.data(), &dsz, (uint8_t*)d_buf.data());
        } else { memcpy(d_buf.data(), c_buf.data(), ch.raw_size); }

        // Distribuiamo i dati decompressi ai rispettivi file
        size_t d_pos = 0;
        while (d_pos < d_buf.size() && current_file_idx < toc.size()) {
            auto& fe = toc[current_file_idx];
            size_t remaining_file = fe.meta.orig_size - block_pos;
            size_t to_write = std::min(remaining_file, d_buf.size() - d_pos);

            if (!test_only) {
                fs::create_directories(fs::path(fe.name).parent_path());
                FILE* out = fopen(fe.name.c_str(), block_pos == 0 ? "wb" : "ab");
                if(out) { fwrite(d_buf.data() + d_pos, 1, to_write, out); fclose(out); }
            }

            d_pos += to_write;
            block_pos += to_write;

            if (block_pos >= fe.meta.orig_size) {
                UI::print_extract(fe.name, fe.meta.orig_size, test_only, true);
                current_file_idx++;
                block_pos = 0;
            }
        }
    }

    ZSTD_freeDCtx(dctx); fclose(f);
    result.ok = true; return result;
}

TarcResult list(const std::string& archive_path) {
    TarcResult result;
    FILE* f = fopen(archive_path.c_str(), "rb");
    Header h; std::vector<FileEntry> toc;
    if (!f || !IO::read_toc(f, h, toc)) { if(f) fclose(f); result.ok = false; return result; }
    for (const auto& fe : toc) {
        UI::print_list_entry(fe.name, fe.meta.orig_size, fe.meta.comp_size, (Codec)fe.meta.codec);
        result.bytes_in += fe.meta.orig_size;
    }
    fclose(f); result.ok = true; return result;
}

TarcResult remove_files(const std::string& archive_path, const std::vector<std::string>& patterns) {
    // Nota: La rimozione in un archivio SOLID richiede la ricostruzione completa. 
    // Per ora restituiamo errore per sicurezza.
    TarcResult r; r.ok = false; r.message = "Rimozione non supportata in modalità SOLID";
    return r;
}

} // namespace Engine
