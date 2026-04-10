#include "engine.h"
#include "io.h"
#include "ui.h"
#include <cstring>
#include <set>
#include <map>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cmath>
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
        ".jpg", ".jpeg", ".avif", ".heic", ".mp4", ".mkv", ".avi", ".mov", ".webm", ".flv",
        ".mp3", ".aac", ".ogg", ".flac", ".opus", ".pptx"
    };

    static const std::set<std::string> TEXT_EXTS = {
        ".txt", ".log", ".ini", ".conf", ".xml", ".json", ".yaml", ".sql",
        ".cpp", ".hpp", ".c", ".h", ".cs", ".py", ".js", ".html", ".css"
    };

    static const std::set<std::string> DB_EXTS = {
        ".mdb", ".accdb", ".mde", ".accde", ".mda", ".mdw"
    };

    Codec select(const std::string& path, int level) {
        std::string ext = fs::path(path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (COMPRESSED_EXTS.count(ext)) return Codec::LZ4;
        if (TEXT_EXTS.count(ext)) return (level >= 15) ? Codec::BR : Codec::LZMA;
        return Codec::ZSTD;
    }
}

namespace Engine {

// Struttura per trasportare i dati compressi tra i thread
struct CompressedChunk {
    uint32_t raw_size;
    uint32_t comp_size;
    std::vector<char> data;
    bool is_compressed;
};

// Funzione atomica di compressione (eseguita nei thread)
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
        if (level > 3) c_sz = LZ4_compress_HC(src.data(), res.data.data(), (int)src.size(), (int)res.data.size(), level);
        else c_sz = LZ4_compress_default(src.data(), res.data.data(), (int)src.size(), (int)res.data.size());
        if (c_sz > 0) { res.comp_size = (uint32_t)c_sz; res.is_compressed = (c_sz < (size_t)res.raw_size); }
    }
    else if (codec == Codec::LZMA) {
        uint32_t preset = std::clamp(level, 0, 9);
        size_t out_pos = 0;
        lzma_ret ret = lzma_easy_buffer_encode(preset, LZMA_CHECK_CRC64, NULL, (const uint8_t*)src.data(), src.size(), (uint8_t*)res.data.data(), &out_pos, res.data.size());
        if (ret == LZMA_OK) { res.comp_size = (uint32_t)out_pos; res.is_compressed = (out_pos < src.size()); }
    }
    else if (codec == Codec::BR) {
        size_t out_sz = res.data.size();
        if (BrotliEncoderCompress(std::min(11, level/2), 19, BROTLI_DEFAULT_MODE, src.size(), (const uint8_t*)src.data(), &out_sz, (uint8_t*)res.data.data())) {
            res.comp_size = (uint32_t)out_sz; res.is_compressed = (out_sz < src.size());
        }
    }

    if (!res.is_compressed) {
        res.data = std::move(src);
        res.comp_size = res.raw_size;
    } else {
        res.data.resize(res.comp_size);
    }
    return res;
}

TarcResult compress(const std::string& archive_path, const std::vector<std::string>& files, bool append, int level) {
    TarcResult result;
    Header h; std::vector<FileEntry> toc;
    std::map<std::string, size_t> existing_map;
    
    FILE* f = fopen(archive_path.c_str(), append && fs::exists(archive_path) ? "rb+" : "wb");
    if (!f) { result.ok = false; result.message = "I/O Error"; return result; }

    if (append && IO::read_toc(f, h, toc)) {
        for (size_t i = 0; i < toc.size(); ++i) existing_map[toc[i].name] = i;
        fseek(f, (long)h.toc_offset, SEEK_SET);
    } else {
        memcpy(h.magic, TARC_MAGIC, 4); h.version = TARC_VERSION;
        IO::write_bytes(f, &h, sizeof(h));
    }

    size_t max_threads = std::max(1u, std::thread::hardware_concurrency());

    for (const auto& path : files) {
        if (!fs::is_regular_file(path)) continue;
        std::string name = fs::relative(path).string();
        uint64_t ts = std::chrono::duration_cast<std::chrono::seconds>(fs::last_write_time(path).time_since_epoch()).count();
        uint64_t sz = fs::file_size(path);

        if (append && existing_map.count(name)) {
            if (toc[existing_map[name]].meta.timestamp == ts) continue;
        }

        FILE* in = fopen(path.c_str(), "rb"); if (!in) continue;
        
        FileEntry fe; fe.name = name; fe.meta.offset = ftell(f); fe.meta.orig_size = sz; fe.meta.timestamp = ts;
        Codec codec = CodecSelector::select(path, level);
        fe.meta.codec = (uint8_t)codec;

        XXH64_state_t* hstate = XXH64_createState(); XXH64_reset(hstate, 0);
        std::queue<std::future<CompressedChunk>> pipeline;
        uint64_t total_comp = 0;
        bool active = true;

        while (active || !pipeline.empty()) {
            // Riempi la pipeline fino al numero di core disponibili
            while (active && pipeline.size() < max_threads) {
                std::vector<char> buf(CHUNK_SIZE);
                size_t n = fread(buf.data(), 1, CHUNK_SIZE, in);
                if (n > 0) {
                    buf.resize(n);
                    XXH64_update(hstate, buf.data(), n);
                    pipeline.push(std::async(std::launch::async, worker_compress, codec, std::move(buf), level));
                } else { active = false; }
            }

            if (!pipeline.empty()) {
                CompressedChunk cc = pipeline.front().get();
                pipeline.pop();
                ChunkHeader ch = { cc.raw_size, cc.comp_size };
                IO::write_bytes(f, &ch, sizeof(ch));
                IO::write_bytes(f, cc.data.data(), cc.comp_size);
                total_comp += cc.comp_size;
                if (!cc.is_compressed) fe.meta.codec = (uint8_t)Codec::NONE;
            }
        }

        ChunkHeader end = {0, 0}; IO::write_bytes(f, &end, sizeof(end));
        fe.meta.comp_size = total_comp; fe.meta.xxhash = XXH64_digest(hstate);
        XXH64_freeState(hstate); fclose(in);

        if (append && existing_map.count(name)) toc[existing_map[name]] = fe;
        else toc.push_back(fe);

        UI::print_add(fe.name, fe.meta.orig_size, (Codec)fe.meta.codec, (float)fe.meta.comp_size / fe.meta.orig_size);
        result.bytes_in += fe.meta.orig_size; result.bytes_out += fe.meta.comp_size;
    }

    IO::write_toc(f, h, toc); fclose(f);
    result.ok = true; return result;
}

TarcResult extract(const std::string& archive_path, bool test_only) {
    TarcResult result;
    FILE* f = fopen(archive_path.c_str(), "rb");
    Header h; std::vector<FileEntry> toc;
    if (!f || !IO::read_toc(f, h, toc)) { if(f) fclose(f); result.ok = false; return result; }
    
    ZSTD_DCtx* dctx = ZSTD_createDCtx();
    std::vector<char> c_buf(CHUNK_SIZE + 65536), d_buf(CHUNK_SIZE);

    for (auto& fe : toc) {
        fseek(f, (long)fe.meta.offset, SEEK_SET);
        FILE* dest = test_only ? nullptr : (fs::create_directories(fs::path(fe.name).parent_path()), fopen(fe.name.c_str(), "wb"));
        XXH64_state_t* hstate = XXH64_createState(); XXH64_reset(hstate, 0);

        while(true) {
            ChunkHeader ch; if (!IO::read_bytes(f, &ch, sizeof(ch)) || ch.raw_size == 0) break;
            IO::read_bytes(f, c_buf.data(), ch.comp_size);

            if (fe.meta.codec == (uint8_t)Codec::ZSTD) ZSTD_decompressDCtx(dctx, d_buf.data(), ch.raw_size, c_buf.data(), ch.comp_size);
            else if (fe.meta.codec == (uint8_t)Codec::LZ4) LZ4_decompress_safe(c_buf.data(), d_buf.data(), ch.comp_size, ch.raw_size);
            else if (fe.meta.codec == (uint8_t)Codec::LZMA) {
                uint64_t mem = 100*1024*1024; size_t ip = 0, op = 0;
                lzma_stream_buffer_decode(&mem, 0, NULL, (uint8_t*)c_buf.data(), &ip, ch.comp_size, (uint8_t*)d_buf.data(), &op, ch.raw_size);
            }
            else if (fe.meta.codec == (uint8_t)Codec::BR) {
                size_t dsz = ch.raw_size;
                BrotliDecoderDecompress(ch.comp_size, (uint8_t*)c_buf.data(), &dsz, (uint8_t*)d_buf.data());
            }
            else memcpy(d_buf.data(), c_buf.data(), ch.raw_size);

            XXH64_update(hstate, d_buf.data(), ch.raw_size);
            if (dest) IO::write_bytes(dest, d_buf.data(), ch.raw_size);
            result.bytes_in += ch.raw_size; result.bytes_out += ch.comp_size;
        }
        UI::print_extract(fe.name, fe.meta.orig_size, test_only, XXH64_digest(hstate) == fe.meta.xxhash);
        if (dest) fclose(dest); XXH64_freeState(hstate);
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
        result.bytes_in += fe.meta.orig_size; result.bytes_out += fe.meta.comp_size;
    }
    fclose(f); result.ok = true; return result;
}

TarcResult remove_files(const std::string& archive_path, const std::vector<std::string>& patterns) {
    TarcResult result;
    FILE* f = fopen(archive_path.c_str(), "rb+");
    Header h; std::vector<FileEntry> toc;
    if (!f || !IO::read_toc(f, h, toc)) { if(f) fclose(f); result.ok = false; return result; }
    std::vector<FileEntry> next;
    for (auto& fe : toc) {
        bool match = false;
        for (auto& p : patterns) if (fe.name.find(p) != std::string::npos) match = true;
        if (match) UI::print_delete(fe.name); else next.push_back(fe);
    }
    if (next.size() < toc.size()) {
        h.toc_offset = ftell(f);
        fseek(f, h.toc_offset, SEEK_SET);
        for (auto& fe : next) IO::write_entry(f, fe);
        fseek(f, 0, SEEK_SET); IO::write_bytes(f, &h, sizeof(h));
    }
    fclose(f); result.ok = true; return result;
}

} // namespace Engine
