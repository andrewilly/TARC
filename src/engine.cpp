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

bool is_database_file(const std::string& path) {
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return DB_EXTS.count(ext) > 0;
}

double calculate_entropy(const char* data, size_t sz) {
    if (sz == 0) return 0.0;
    size_t freq[256] = {0};
    for (size_t i = 0; i < sz; ++i) freq[(uint8_t)data[i]]++;
    double entropy = 0.0;
    for (size_t i = 0; i < 256; ++i) {
        if (freq[i] > 0) {
            double p = (double)freq[i] / sz;
            entropy -= p * (std::log2(p));
        }
    }
    return entropy;
}

Codec select_auto(const std::string& path, const char* sample_data = nullptr, size_t sample_sz = 0) {
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (sample_data && sample_sz > 0) {
        if (calculate_entropy(sample_data, sample_sz) > 7.5) return Codec::NONE;
    }
    if (COMPRESSED_EXTS.count(ext)) return Codec::LZ4;
    if (TEXT_EXTS.count(ext))       return Codec::LZMA;
    return Codec::ZSTD; 
}
}

namespace Engine {

struct ChunkResult {
    bool ok;
    size_t comp_size;
};

ChunkResult compress_chunk(Codec codec, ZSTD_CCtx* zctx, const void* src, size_t src_sz, void* dst, size_t dst_cap, int level) {
    size_t c_sz = 0;
    if (codec == Codec::ZSTD) {
        c_sz = ZSTD_compressCCtx(zctx, dst, dst_cap, src, src_sz, level);
        if (ZSTD_isError(c_sz)) return {false, 0};
    } else if (codec == Codec::LZ4) {
        c_sz = (size_t)LZ4_compress_default((const char*)src, (char*)dst, (int)src_sz, (int)dst_cap);
        if (c_sz <= 0) return {false, 0};
    } else if (codec == Codec::LZMA) {
        size_t out_pos = 0;
        lzma_ret ret = lzma_easy_buffer_encode((uint32_t)level, LZMA_CHECK_CRC64, nullptr, (const uint8_t*)src, src_sz, (uint8_t*)dst, &out_pos, dst_cap);
        if (ret != LZMA_OK) return {false, 0};
        c_sz = out_pos;
    } else {
        memcpy(dst, src, src_sz);
        c_sz = src_sz;
    }
    return {true, c_sz};
}

TarcResult compress(const std::string& arch_path, const std::vector<std::string>& files, bool append, int level) {
    TarcResult result;
    Header h = {{'T','A','R','C'}, TARC_VERSION, 0};
    std::vector<FileEntry> toc;
    std::map<std::string, FileEntry> existing_files;

    if (append && fs::exists(arch_path)) {
        FILE* f_old = fopen(arch_path.c_str(), "rb");
        if(f_old) {
            Header old_h;
            if (IO::read_toc(f_old, old_h, toc)) {
                for(auto& fe : toc) existing_files[fe.name] = fe;
            }
            fclose(f_old);
        }
    }

    FILE* out = fopen(arch_path.c_str(), append ? "rb+" : "wb");
    if (!out) { result.ok = false; result.message = "ERRORE IO"; return result; }
    if (append) fseek(out, 0, SEEK_END);
    else IO::write_bytes(out, &h, sizeof(h));

    ZSTD_CCtx* zctx = ZSTD_createCCtx();
    std::vector<char> in_buf(CHUNK_SIZE);
    std::vector<char> out_buf(CHUNK_SIZE + 65536);

    for (const auto& fpath : files) {
        if (!fs::exists(fpath) || fs::is_directory(fpath)) continue;
        uint64_t ts = (uint64_t)fs::last_write_time(fpath).time_since_epoch().count();
        uint64_t cur_sz = (uint64_t)fs::file_size(fpath);

        if (existing_files.count(fpath)) {
            if (existing_files[fpath].meta.timestamp == ts && existing_files[fpath].meta.orig_size == cur_sz) {
                UI::print_warning("Già aggiornato: " + fpath);
                continue;
            }
            toc.erase(std::remove_if(toc.begin(), toc.end(), [&](const FileEntry& e){ return e.name == fpath; }), toc.end());
        }

        FILE* src = fopen(fpath.c_str(), "rb");
        if (!src) continue;
        size_t first_read = fread(in_buf.data(), 1, 4096, src);
        Codec codec = CodecSelector::select_auto(fpath, in_buf.data(), first_read);
        rewind(src);

        int eff_level = (codec == Codec::ZSTD && CodecSelector::is_database_file(fpath)) ? std::min(level + 3, 22) : level;
        uint64_t f_offset = (uint64_t)ftell(out);
        XXH64_state_t* hash_state = XXH64_createState(); XXH64_reset(hash_state, 0);
        uint64_t total_in = 0, total_out = 0;

        while (true) {
            size_t r = fread(in_buf.data(), 1, CHUNK_SIZE, src);
            if (r == 0) break;
            XXH64_update(hash_state, in_buf.data(), r);
            auto res = compress_chunk(codec, zctx, in_buf.data(), r, out_buf.data(), out_buf.size(), eff_level);
            ChunkHeader ch = {(uint32_t)r, (uint32_t)res.comp_size};
            IO::write_bytes(out, &ch, sizeof(ch));
            IO::write_bytes(out, out_buf.data(), res.comp_size);
            total_in += r; total_out += (res.comp_size + sizeof(ch));
        }
        ChunkHeader stop = {0, 0}; IO::write_bytes(out, &stop, sizeof(stop));
        total_out += sizeof(stop);
        toc.push_back({{f_offset, total_in, total_out, XXH64_digest(hash_state), ts, (uint16_t)fpath.size(), (uint8_t)codec, 0}, fpath});
        UI::print_add(fpath, total_in, codec, 1.0f - (float)total_out/total_in);
        XXH64_freeState(hash_state); fclose(src);
    }
    IO::write_toc(out, h, toc); fclose(out);
    ZSTD_freeCCtx(zctx);
    result.ok = true; return result;
}

TarcResult list(const std::string& arch_path) {
    TarcResult result;
    FILE* f = fopen(arch_path.c_str(), "rb");
    Header h; std::vector<FileEntry> toc;
    if (!IO::read_toc(f, h, toc)) { if(f) fclose(f); result.ok = false; return result; }
    for (auto& fe : toc) {
        UI::print_list_entry(fe.name, fe.meta.orig_size, fe.meta.comp_size, (Codec)fe.meta.codec);
        result.bytes_in += fe.meta.orig_size; result.bytes_out += fe.meta.comp_size;
    }
    fclose(f); return result;
}

TarcResult extract(const std::string& arch_path, bool test_only) {
    TarcResult result;
    FILE* f = fopen(arch_path.c_str(), "rb");
    Header h; std::vector<FileEntry> toc;
    if (!IO::read_toc(f, h, toc)) { if(f) fclose(f); result.ok = false; return result; }
    ZSTD_DCtx* dctx = ZSTD_createDCtx();
    std::vector<char> c_buf(CHUNK_SIZE + 65536);
    std::vector<char> d_buf(CHUNK_SIZE);
    for (auto& fe : toc) {
        fseek(f, (long)fe.meta.offset, SEEK_SET);
        FILE* dest = test_only ? nullptr : fopen(fe.name.c_str(), "wb");
        XXH64_state_t* hstate = XXH64_createState(); XXH64_reset(hstate, 0);
        while(true) {
            ChunkHeader ch; IO::read_bytes(f, &ch, sizeof(ch));
            if (ch.raw_size == 0) break;
            IO::read_bytes(f, c_buf.data(), ch.comp_size);
            if (fe.meta.codec == (uint8_t)Codec::ZSTD) ZSTD_decompressDCtx(dctx, d_buf.data(), ch.raw_size, c_buf.data(), ch.comp_size);
            else memcpy(d_buf.data(), c_buf.data(), ch.raw_size);
            XXH64_update(hstate, d_buf.data(), ch.raw_size);
            if (dest) IO::write_bytes(dest, d_buf.data(), ch.raw_size);
        }
        bool integrity = (XXH64_digest(hstate) == fe.meta.xxhash);
        UI::print_extract(fe.name, fe.meta.orig_size, test_only, integrity);
        if (dest) fclose(dest); XXH64_freeState(hstate);
    }
    ZSTD_freeDCtx(dctx); fclose(f);
    return result;
}

TarcResult remove_files(const std::string& arch_path, const std::vector<std::string>& targets) {
    TarcResult result; // Funzione stub per compatibilità main
    result.ok = true; return result;
}

} // Engine
