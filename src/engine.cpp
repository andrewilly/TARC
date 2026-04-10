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
#include <climits>

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
    ".jpg", ".jpeg", ".avif", ".heic",
    ".mp4", ".mkv", ".avi", ".mov", ".webm", ".flv",
    ".mp3", ".aac", ".ogg", ".flac", ".opus", ".pptx"
};

static const std::set<std::string> TEXT_EXTS = {
    ".txt", ".log", ".ini", ".conf", ".xml", ".json", ".yaml", ".sql",
    ".cpp", ".hpp", ".c", ".h", ".cs", ".py", ".js", ".html", ".css"
};

static const std::set<std::string> DB_EXTS = {
    ".mdb", ".accdb", ".mde", ".accde", ".mda", ".mdw"
};

[[maybe_unused]] static double calculate_entropy(const uint8_t* data, size_t size) {
    if (size == 0) return 0;
    size_t counts[256] = {0};
    for (size_t i = 0; i < size; ++i) counts[data[i]]++;
    double ent = 0;
    for (size_t i = 0; i < 256; ++i) {
        if (counts[i] > 0) {
            double p = (double)counts[i] / size;
            ent -= p * log2(p);
        }
    }
    return ent;
}

Codec select(const std::string& path, int level) {
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (COMPRESSED_EXTS.count(ext)) return Codec::LZ4;
    
    if (TEXT_EXTS.count(ext)) {
        if (level >= 15) return Codec::BR; 
        return Codec::LZMA;
    }
    
    return Codec::ZSTD;
}

} // namespace CodecSelector

namespace Engine {

// ─── HELPER COMPRESSIONE CHUNK ───────────────────────────────────────────────
static std::pair<bool, size_t> compress_chunk(Codec codec, const void* src, size_t src_sz, void* dst, size_t dst_cap, int level) {
    size_t c_sz = 0;
    
    if (codec == Codec::ZSTD) {
        c_sz = ZSTD_compress(dst, dst_cap, src, src_sz, level);
        if (ZSTD_isError(c_sz)) return {false, 0};
    } 
    else if (codec == Codec::LZ4) {
        if (level > 3) c_sz = LZ4_compress_HC((const char*)src, (char*)dst, (int)src_sz, (int)dst_cap, level);
        else c_sz = LZ4_compress_default((const char*)src, (char*)dst, (int)src_sz, (int)dst_cap);
        if (c_sz <= 0) return {false, 0};
    }
    else if (codec == Codec::LZMA) {
        uint32_t preset = (level > 9) ? 9 : (level < 0 ? 0 : level); 
        size_t out_pos = 0;
        lzma_ret ret = lzma_easy_buffer_encode(preset, LZMA_CHECK_CRC64, NULL, 
                                               (const uint8_t*)src, src_sz, 
                                               (uint8_t*)dst, &out_pos, dst_cap);
        if (ret != LZMA_OK) return {false, 0};
        c_sz = out_pos;
    }
    else if (codec == Codec::BR) {
        size_t out_sz = dst_cap;
        int br_qual = std::min(11, level / 2);
        if (!BrotliEncoderCompress(br_qual, BROTLI_DEFAULT_WINDOW, BROTLI_DEFAULT_MODE,
                                   src_sz, (const uint8_t*)src, &out_sz, (uint8_t*)dst)) {
            return {false, 0};
        }
        c_sz = out_sz;
    }
    else {
        return {false, 0};
    }
    
    return {true, c_sz};
}

static uint64_t get_file_timestamp(const std::string& path) {
    try {
        auto ftime = fs::last_write_time(path);
        return std::chrono::duration_cast<std::chrono::seconds>(ftime.time_since_epoch()).count();
    } catch (...) { return 0; }
}

// ─── COMPRESS (ex create) ────────────────────────────────────────────────────
TarcResult compress(const std::string& archive_path, const std::vector<std::string>& files, bool append, int level) {
    TarcResult result;
    Header h; 
    std::vector<FileEntry> toc;
    std::map<std::string, size_t> existing_map;

    FILE* f = nullptr;
    if (append && fs::exists(archive_path)) {
        f = fopen(archive_path.c_str(), "rb+");
        if (f && IO::read_toc(f, h, toc)) {
            for (size_t i = 0; i < toc.size(); ++i) existing_map[toc[i].name] = i;
            fseek(f, (long)h.toc_offset, SEEK_SET);
        } else {
            if (f) fclose(f);
            f = fopen(archive_path.c_str(), "wb");
            memcpy(h.magic, TARC_MAGIC, 4);
            h.version = TARC_VERSION;
            IO::write_bytes(f, &h, sizeof(h));
        }
    } else {
        f = fopen(archive_path.c_str(), "wb");
        memcpy(h.magic, TARC_MAGIC, 4);
        h.version = TARC_VERSION;
        IO::write_bytes(f, &h, sizeof(h));
    }

    if (!f) { result.ok = false; result.message = "Impossibile aprire l'archivio"; return result; }

    std::vector<char> src_buf(CHUNK_SIZE);
    std::vector<char> comp_buf(CHUNK_SIZE + 65536);

    for (const auto& path : files) {
        if (!fs::is_regular_file(path)) continue;

        uint64_t ts = get_file_timestamp(path);
        uint64_t sz = fs::file_size(path);
        std::string name = fs::relative(path).string();

        if (append && existing_map.count(name)) {
            auto& old = toc[existing_map[name]];
            if (old.meta.timestamp == ts && old.meta.orig_size == sz) continue;
        }

        FILE* in = fopen(path.c_str(), "rb");
        if (!in) continue;

        FileEntry fe;
        fe.name = name;
        fe.meta.offset = (uint64_t)ftell(f);
        fe.meta.orig_size = sz;
        fe.meta.timestamp = ts;
        
        Codec codec = CodecSelector::select(path, level);
        int active_level = level;
        std::string ext = fs::path(path).extension().string();
        if (CodecSelector::DB_EXTS.count(ext)) {
            codec = Codec::ZSTD;
            active_level = std::min(22, level + 3);
        }
        fe.meta.codec = (uint8_t)codec;

        XXH64_state_t* hstate = XXH64_createState();
        XXH64_reset(hstate, 0);

        uint64_t total_comp = 0;
        while (true) {
            size_t n = fread(src_buf.data(), 1, CHUNK_SIZE, in);
            if (n == 0) break;
            XXH64_update(hstate, src_buf.data(), n);

            ChunkHeader ch;
            ch.raw_size = (uint32_t)n;
            
            auto res = compress_chunk(codec, src_buf.data(), n, comp_buf.data(), comp_buf.size(), active_level);
            
            if (res.first && res.second < n) {
                ch.comp_size = (uint32_t)res.second;
                IO::write_bytes(f, &ch, sizeof(ch));
                IO::write_bytes(f, comp_buf.data(), ch.comp_size);
                total_comp += ch.comp_size;
            } else {
                ch.comp_size = ch.raw_size;
                fe.meta.codec = (uint8_t)Codec::NONE;
                IO::write_bytes(f, &ch, sizeof(ch));
                IO::write_bytes(f, src_buf.data(), ch.raw_size);
                total_comp += ch.raw_size;
            }
        }
        
        ChunkHeader end = {0, 0};
        IO::write_bytes(f, &end, sizeof(end));

        fe.meta.comp_size = total_comp;
        fe.meta.xxhash = XXH64_digest(hstate);
        XXH64_freeState(hstate);
        fclose(in);

        if (append && existing_map.count(name)) toc[existing_map[name]] = fe;
        else toc.push_back(fe);

        UI::print_add(fe.name, fe.meta.orig_size, (Codec)fe.meta.codec, 
                     fe.meta.orig_size > 0 ? (float)fe.meta.comp_size / fe.meta.orig_size : 1.0f);
        
        result.bytes_in += fe.meta.orig_size;
        result.bytes_out += fe.meta.comp_size;
    }

    IO::write_toc(f, h, toc);
    fclose(f);
    result.ok = true;
    return result;
}

// ─── EXTRACT / TEST ──────────────────────────────────────────────────────────
TarcResult extract(const std::string& archive_path, bool test_only) {
    TarcResult result;
    FILE* f = fopen(archive_path.c_str(), "rb");
    Header h; 
    std::vector<FileEntry> toc;
    if (!f || !IO::read_toc(f, h, toc)) { if(f) fclose(f); result.ok = false; result.message = "Archivio non valido"; return result; }
    
    ZSTD_DCtx* dctx = ZSTD_createDCtx();
    std::vector<char> c_buf(CHUNK_SIZE + 65536);
    std::vector<char> d_buf(CHUNK_SIZE);

    for (auto& fe : toc) {
        fseek(f, (long)fe.meta.offset, SEEK_SET);
        FILE* dest = nullptr;
        if (!test_only) {
            fs::path p(fe.name);
            if (p.has_parent_path()) fs::create_directories(p.parent_path());
            dest = fopen(fe.name.c_str(), "wb");
        }

        XXH64_state_t* hstate = XXH64_createState();
        XXH64_reset(hstate, 0);

        while(true) {
            ChunkHeader ch; 
            if (!IO::read_bytes(f, &ch, sizeof(ch)) || ch.raw_size == 0) break;
            IO::read_bytes(f, c_buf.data(), ch.comp_size);

            if (fe.meta.codec == (uint8_t)Codec::ZSTD) {
                ZSTD_decompressDCtx(dctx, d_buf.data(), ch.raw_size, c_buf.data(), ch.comp_size);
            } 
            else if (fe.meta.codec == (uint8_t)Codec::LZ4) {
                LZ4_decompress_safe(c_buf.data(), d_buf.data(), (int)ch.comp_size, (int)ch.raw_size);
            }
            else if (fe.meta.codec == (uint8_t)Codec::LZMA) {
                uint64_t memlimit = 100 * 1024 * 1024;
                size_t in_pos = 0, out_pos = 0;
                lzma_ret ret = lzma_stream_buffer_decode(&memlimit, 0, NULL, (const uint8_t*)c_buf.data(), 
                                                         &in_pos, ch.comp_size, (uint8_t*)d_buf.data(), 
                                                         &out_pos, ch.raw_size);
                (void)ret;
            }
            else if (fe.meta.codec == (uint8_t)Codec::BR) {
                size_t decoded_sz = ch.raw_size;
                BrotliDecoderDecompress(ch.comp_size, (const uint8_t*)c_buf.data(), 
                                        &decoded_sz, (uint8_t*)d_buf.data());
            }
            else {
                memcpy(d_buf.data(), c_buf.data(), ch.raw_size);
            }

            XXH64_update(hstate, d_buf.data(), ch.raw_size);
            if (dest) IO::write_bytes(dest, d_buf.data(), ch.raw_size);
            
            result.bytes_in += ch.raw_size;
            result.bytes_out += ch.comp_size;
        }

        uint64_t final_hash = XXH64_digest(hstate);
        bool hash_ok = (final_hash == fe.meta.xxhash);
        UI::print_extract(fe.name, fe.meta.orig_size, test_only, hash_ok);
        
        if (dest) fclose(dest);
        XXH64_freeState(hstate);
    }

    ZSTD_freeDCtx(dctx);
    fclose(f);
    result.ok = true;
    return result;
}

TarcResult list(const std::string& archive_path) {
    TarcResult result;
    FILE* f = fopen(archive_path.c_str(), "rb");
    Header h; std::vector<FileEntry> toc;
    if (!f || !IO::read_toc(f, h, toc)) { if(f) fclose(f); result.ok = false; return result; }
    for (const auto& fe : toc) {
        UI::print_list_entry(fe.name, fe.meta.orig_size, fe.meta.comp_size, (Codec)fe.meta.codec);
        result.bytes_in += fe.meta.orig_size;
        result.bytes_out += fe.meta.comp_size;
    }
    fclose(f);
    result.ok = true;
    return result;
}

// ─── REMOVE_FILES (ex remove) ────────────────────────────────────────────────
TarcResult remove_files(const std::string& archive_path, const std::vector<std::string>& patterns) {
    TarcResult result;
    FILE* f = fopen(archive_path.c_str(), "rb+");
    if (!f) { result.ok = false; result.message = "Impossibile aprire l'archivio"; return result; }

    Header h; std::vector<FileEntry> toc;
    if (!IO::read_toc(f, h, toc)) { fclose(f); result.ok = false; return result; }

    std::vector<FileEntry> next_toc;
    for (auto& fe : toc) {
        bool keep = true;
        for (auto& p : patterns) {
            if (fe.name.find(p) != std::string::npos) {
                keep = false;
                UI::print_delete(fe.name);
                break;
            }
        }
        if (keep) next_toc.push_back(fe);
    }

    if (next_toc.size() == toc.size()) {
        fclose(f);
        result.ok = true;
        result.message = "Nessun file rimosso.";
        return result;
    }

    h.toc_offset = ftell(f); 
    fseek(f, h.toc_offset, SEEK_SET);
    
    for (auto& fe : next_toc) {
        IO::write_entry(f, fe);
    }
    
    fseek(f, 0, SEEK_SET);
    IO::write_bytes(f, &h, sizeof(h));

    fclose(f);
    result.ok = true;
    return result;
}

} // namespace Engine
