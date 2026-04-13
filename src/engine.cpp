#include "engine.h"
#include "io.h"
#include "ui.h"
#include "types.h"
#include <cstring>
#include <map>
#include <filesystem>
#include <fstream>
#include <vector>
#include <iostream>
#include <chrono>
#include <future>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/sysinfo.h>
#endif

#include "zstd.h"
#include "lzma.h"

extern "C" {
    #include "xxhash.h"
}

namespace fs = std::filesystem;

namespace Engine {

// --- Rilevamento RAM di sistema ---
size_t get_system_ram() {
#ifdef _WIN32
    MEMORYSTATUSEX status;
    status.dwLength = sizeof(status);
    GlobalMemoryStatusEx(&status);
    return (size_t)status.ullTotalPhys;
#else
    struct sysinfo si;
    if (sysinfo(&si) == 0) return (size_t)si.totalram * si.mem_unit;
    return 2048ULL * 1024 * 1024; // Default 2GB
#endif
}

// Struttura per il worker multithread
struct ChunkResult {
    std::vector<char> compressed_data;
    uint32_t raw_size;
    bool success;
};

// Worker di compressione LZMA
ChunkResult compress_worker(std::vector<char> raw_data, int level) {
    ChunkResult res;
    res.raw_size = (uint32_t)raw_data.size();
    res.compressed_data.resize(raw_data.size() + (128 * 1024));
    
    lzma_options_lzma opt;
    lzma_lzma_preset(&opt, (uint32_t)((level < 0) ? 6 : level));
    lzma_filter filters[] = {{ LZMA_FILTER_LZMA2, &opt }, { LZMA_VLI_UNKNOWN, NULL }};
    size_t out_pos = 0;
    
    lzma_ret ret = lzma_stream_buffer_encode(filters, LZMA_CHECK_CRC64, NULL, 
                                           (uint8_t*)raw_data.data(), raw_data.size(), 
                                           (uint8_t*)res.compressed_data.data(), &out_pos, res.compressed_data.size());
    
    if (ret == LZMA_OK) {
        res.compressed_data.resize(out_pos);
        res.success = true;
    } else {
        res.success = false;
    }
    return res;
}

TarcResult compress(const std::string& archive_path, const std::vector<std::string>& files, bool append, int level) {
    (void)append;
    TarcResult result;
    ::Header h;
    std::vector<::FileEntry> final_toc;
    std::map<uint64_t, uint32_t> hash_map;

    // Calcolo dinamico soglia Chunk (15% RAM)
    size_t sys_ram = get_system_ram();
    size_t CHUNK_THRESHOLD = sys_ram / 7; 
    if (CHUNK_THRESHOLD < 32 * 1024 * 1024) CHUNK_THRESHOLD = 32 * 1024 * 1024;
    if (CHUNK_THRESHOLD > 512 * 1024 * 1024) CHUNK_THRESHOLD = 512 * 1024 * 1024;

    FILE* f = fopen((archive_path + ".tmp").c_str(), "wb");
    if (!f) return {false, "Errore apertura file"};

    memset(&h, 0, sizeof(h));
    memcpy(h.magic, "STRK", 4);
    h.version = 110;
    fwrite(&h, sizeof(h), 1, f);

    std::vector<char> solid_buf;
    std::future<ChunkResult> future_chunk;
    bool worker_active = false;

    auto write_finished_worker = [&](std::future<ChunkResult>& fut) {
        ChunkResult res = fut.get();
        if (res.success) {
            ::ChunkHeader ch = { res.raw_size, (uint32_t)res.compressed_data.size() };
            fwrite(&ch, sizeof(ch), 1, f);
            fwrite(res.compressed_data.data(), 1, res.compressed_data.size(), f);
            result.bytes_out += res.compressed_data.size();
        }
    };

    for (size_t i = 0; i < files.size(); ++i) {
        if (!fs::is_regular_file(files[i])) continue;
        UI::print_progress(i + 1, files.size(), fs::path(files[i]).filename().string());

        uintmax_t fsize = fs::file_size(files[i]);
        std::ifstream in(files[i], std::ios::binary);
        std::vector<char> data(fsize);
        in.read(data.data(), fsize);
        in.close();

        uint64_t h64 = XXH64(data.data(), fsize, 0);
        
        ::FileEntry fe;
        fe.name = fs::relative(files[i]).string();
        fe.meta.orig_size = (uint32_t)fsize;
        fe.meta.xxhash = h64;
        fe.meta.codec = (uint8_t)Codec::LZMA;
        auto ftime = fs::last_write_time(files[i]);
        fe.meta.timestamp = std::chrono::duration_cast<std::chrono::seconds>(ftime.time_since_epoch()).count();

        if (hash_map.count(h64)) {
            fe.meta.is_duplicate = 1;
            fe.meta.duplicate_of_idx = hash_map[h64];
        } else {
            if (!solid_buf.empty() && (solid_buf.size() + fsize > CHUNK_THRESHOLD)) {
                if (worker_active) write_finished_worker(future_chunk);
                future_chunk = std::async(std::launch::async, compress_worker, std::move(solid_buf), level);
                worker_active = true;
                solid_buf.clear();
            }
            hash_map[h64] = (uint32_t)final_toc.size();
            fe.meta.is_duplicate = 0;
            solid_buf.insert(solid_buf.end(), data.begin(), data.end());
            result.bytes_in += fsize;
        }
        final_toc.push_back(fe);
    }

    if (worker_active) write_finished_worker(future_chunk);
    if (!solid_buf.empty()) {
        ChunkResult last_res = compress_worker(std::move(solid_buf), level);
        ::ChunkHeader ch = { last_res.raw_size, (uint32_t)last_res.compressed_data.size() };
        fwrite(&ch, sizeof(ch), 1, f);
        fwrite(last_res.compressed_data.data(), 1, last_res.compressed_data.size(), f);
        result.bytes_out += last_res.compressed_data.size();
    }

    ::ChunkHeader end_mark = {0, 0};
    fwrite(&end_mark, sizeof(end_mark), 1, f);
    
    IO::write_toc(f, h, final_toc);
    fclose(f);
    fs::rename(archive_path + ".tmp", archive_path);
    
    result.ok = true;
    return result;
}

TarcResult extract(const std::string& arch_path, bool test_only) {
    TarcResult result;
    FILE* f = fopen(arch_path.c_str(), "rb");
    if (!f) return {false, "File non trovato"};

    ::Header h;
    if (fread(&h, sizeof(h), 1, f) != 1) { fclose(f); return {false, "Header corrotto"}; }

    std::vector<::FileEntry> toc;
    if (!IO::read_toc(f, h, toc)) { fclose(f); return {false, "TOC non leggibile"}; }

    fseek(f, sizeof(::Header), SEEK_SET);
    std::vector<char> current_block;
    size_t block_pos = 0;

    for (size_t i = 0; i < toc.size(); ++i) {
        auto& fe = toc[i];
        UI::print_progress(i + 1, toc.size(), fe.name);

        if (fe.meta.is_duplicate) {
            if (!test_only) {
                try { fs::copy(toc[fe.meta.duplicate_of_idx].name, fe.name, fs::copy_options::overwrite_existing); } catch(...) {}
            }
            continue;
        }

        if (block_pos >= current_block.size()) {
            ::ChunkHeader ch;
            if (fread(&ch, sizeof(ch), 1, f) == 1 && ch.raw_size > 0) {
                std::vector<char> comp(ch.comp_size);
                fread(comp.data(), 1, ch.comp_size, f);
                current_block.resize(ch.raw_size);
                size_t sp = 0, dp = 0;
                lzma_stream_buffer_decode(NULL, UINT64_MAX, NULL, (uint8_t*)comp.data(), &sp, ch.comp_size, (uint8_t*)current_block.data(), &dp, ch.raw_size);
                block_pos = 0;
            }
        }

        if (block_pos + fe.meta.orig_size > current_block.size()) {
            fclose(f); return {false, "Sincronizzazione fallita su: " + fe.name};
        }

        const char* data_ptr = current_block.data() + block_pos;
        if (XXH64(data_ptr, fe.meta.orig_size, 0) != fe.meta.xxhash) {
            fclose(f); return {false, "ERRORE INTEGRITÀ: " + fe.name};
        }

        if (!test_only) IO::write_file_to_disk(fe.name, data_ptr, fe.meta.orig_size, fe.meta.timestamp);
        
        block_pos += fe.meta.orig_size;
        result.bytes_out += fe.meta.orig_size;
    }

    fclose(f);
    result.ok = true;
    return result;
}

TarcResult list(const std::string& arch_path) {
    TarcResult res;
    FILE* f = fopen(arch_path.c_str(), "rb");
    if (!f) return {false, "Errore"};
    ::Header h; fread(&h, sizeof(h), 1, f);
    std::vector<::FileEntry> toc;
    IO::read_toc(f, h, toc);
    std::cout << "\n--- ARCHIVIO SOLID (.strk) ---\n";
    for (const auto& fe : toc) UI::print_list_entry(fe.name, fe.meta.orig_size, 0, (Codec)fe.meta.codec);
    fclose(f);
    res.ok = true;
    return res;
}

TarcResult remove_files(const std::string& a, const std::vector<std::string>& p) { return {false, "Non disponibile"}; }

} // namespace Engine
