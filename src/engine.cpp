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
#include <queue>
#include <regex>

#ifdef _WIN32
    #include <windows.h>
#elif defined(__APPLE__)
    #include <sys/types.h>
    #include <sys/sysctl.h>
    #include <unistd.h>
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

// Helper interno per il confronto wildcard (usato in estrazione selettiva)
bool wildcard_match(const std::string& text, const std::string& pattern) {
    if (pattern == "*" || pattern == "*.*") return true;
    try {
        std::string r = std::regex_replace(pattern, std::regex("\\."), "\\.");
        r = std::regex_replace(r, std::regex("\\*"), ".*");
        r = std::regex_replace(r, std::regex("\\?"), ".");
        std::regex re(r, std::regex_constants::icase);
        return std::regex_match(text, re);
    } catch (...) { return text == pattern; }
}

size_t get_system_ram() {
#ifdef _WIN32
    MEMORYSTATUSEX status;
    status.dwLength = sizeof(status);
    GlobalMemoryStatusEx(&status);
    return (size_t)status.ullTotalPhys;
#elif defined(__APPLE__)
    int64_t mem;
    size_t len = sizeof(mem);
    static int mib[2] = { CTL_HW, HW_MEMSIZE };
    if (sysctl(mib, 2, &mem, &len, NULL, 0) == 0) {
        return (size_t)mem;
    }
    return 2048ULL * 1024 * 1024;
#else
    struct sysinfo si;
    if (sysinfo(&si) == 0) return (size_t)si.totalram * si.mem_unit;
    return 2048ULL * 1024 * 1024;
#endif
}

struct ChunkResult {
    std::vector<char> compressed_data;
    uint32_t raw_size;
    bool success;
    std::vector<size_t> file_indices; 
};

ChunkResult compress_worker(std::vector<char> raw_data, int level, std::vector<size_t> indices) {
    ChunkResult res;
    res.raw_size = (uint32_t)raw_data.size();
    res.file_indices = indices;
    
    size_t max_out = lzma_stream_buffer_bound(raw_data.size());
    res.compressed_data.resize(max_out);

    size_t out_pos = 0;
    uint32_t preset = (level < 0) ? 6 : (uint32_t)level;
    
    lzma_ret ret = lzma_easy_buffer_encode(
        preset, LZMA_CHECK_CRC64, NULL, 
        (const uint8_t*)raw_data.data(), raw_data.size(), 
        (uint8_t*)res.compressed_data.data(), &out_pos, max_out);
    
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

    try {
        fs::space_info space = fs::space(fs::path(archive_path).parent_path());
        uintmax_t total_input_size = 0;
        for (const auto& f : files) {
            if (fs::is_regular_file(f))
                total_input_size += fs::file_size(f);
        }
        if (space.available < total_input_size * 2) {
            return {false, "Spazio su disco insufficiente"};
        }
    } catch (...) {}

    size_t sys_ram = get_system_ram();
    size_t CHUNK_THRESHOLD = sys_ram / 7; 
    if (CHUNK_THRESHOLD < 32 * 1024 * 1024) CHUNK_THRESHOLD = 32 * 1024 * 1024;
    if (CHUNK_THRESHOLD > 512 * 1024 * 1024) CHUNK_THRESHOLD = 512 * 1024 * 1024;

    std::string temp_path = archive_path + ".tmp";
    FILE* f = fopen(temp_path.c_str(), "wb");
    if (!f) return {false, "Errore apertura file temporaneo"};

    memset(&h, 0, sizeof(h));
    memcpy(h.magic, "STRK", 4);
    h.version = 110;
    if (fwrite(&h, sizeof(h), 1, f) != 1) { 
        fclose(f); 
        fs::remove(temp_path);
        return {false, "Errore scrittura header"}; 
    }

    std::vector<char> solid_buf;
    std::vector<size_t> current_chunk_indices;
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
        if (fsize > 0) in.read(data.data(), fsize);
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
            final_toc.push_back(fe);
            continue; 
        }
        
        hash_map[h64] = (uint32_t)final_toc.size();
        fe.meta.is_duplicate = 0;
        
        if (!solid_buf.empty() && (solid_buf.size() + fsize > CHUNK_THRESHOLD)) {
            if (worker_active) write_finished_worker(future_chunk);
            future_chunk = std::async(std::launch::async, compress_worker, std::move(solid_buf), level, current_chunk_indices);
            worker_active = true;
            solid_buf.clear();
            current_chunk_indices.clear();
        }
        
        solid_buf.insert(solid_buf.end(), data.begin(), data.end());
        current_chunk_indices.push_back(final_toc.size());
        result.bytes_in += fsize;
        final_toc.push_back(fe);
    }

    if (worker_active) write_finished_worker(future_chunk);
    if (!solid_buf.empty()) {
        ChunkResult last_res = compress_worker(std::move(solid_buf), level, current_chunk_indices);
        if (last_res.success) {
            ::ChunkHeader ch = { last_res.raw_size, (uint32_t)last_res.compressed_data.size() };
            fwrite(&ch, sizeof(ch), 1, f);
            fwrite(last_res.compressed_data.data(), 1, last_res.compressed_data.size(), f);
            result.bytes_out += last_res.compressed_data.size();
        }
    }

    ::ChunkHeader end_mark = {0, 0};
    fwrite(&end_mark, sizeof(end_mark), 1, f);
    IO::write_toc(f, h, final_toc);
    fclose(f);
    fs::rename(temp_path, archive_path);
    result.ok = true;
    return result;
}

// Estrazione aggiornata con supporto filtri/wildcard
TarcResult extract(const std::string& arch_path, const std::vector<std::string>& patterns, bool test_only) {
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
    bool block_valid = false;

    for (size_t i = 0; i < toc.size(); ++i) {
        auto& fe = toc[i];

        // LOGICA DI FILTRO
        bool should_extract = true;
        if (!patterns.empty()) {
            should_extract = false;
            for (const auto& p : patterns) {
                if (wildcard_match(fe.name, p)) { should_extract = true; break; }
            }
        }

        if (fe.meta.is_duplicate) {
            if (should_extract && !test_only) {
                try { 
                    if (fe.meta.duplicate_of_idx < i) 
                        fs::copy(toc[fe.meta.duplicate_of_idx].name, fe.name, fs::copy_options::overwrite_existing);
                } catch(...) {}
            }
            if (should_extract) result.bytes_out += fe.meta.orig_size;
            continue;
        }

        // Se non è un duplicato, dobbiamo comunque gestire il chunk per mantenere la sincronia Solid
        if (!block_valid || block_pos >= current_block.size()) {
            ::ChunkHeader ch;
            if (fread(&ch, sizeof(ch), 1, f) != 1 || ch.raw_size == 0) {
                fclose(f); return {false, "Errore chunk su: " + fe.name};
            }
            std::vector<char> comp(ch.comp_size);
            fread(comp.data(), 1, ch.comp_size, f);
            
            current_block.assign(ch.raw_size, 0);
            size_t src_pos = 0, dst_pos = 0;
            uint64_t memlimit = UINT64_MAX;
            lzma_stream_buffer_decode(&memlimit, 0, NULL, (uint8_t*)comp.data(), &src_pos, ch.comp_size, (uint8_t*)current_block.data(), &dst_pos, ch.raw_size);
            block_pos = 0;
            block_valid = true;
        }

        if (should_extract) {
            UI::print_progress(i + 1, toc.size(), fe.name);
            const char* data_ptr = current_block.data() + block_pos;
            
            if (XXH64(data_ptr, fe.meta.orig_size, 0) != fe.meta.xxhash) {
                fclose(f); return {false, "ERRORE INTEGRITÀ: " + fe.name};
            }

            if (!test_only) IO::write_file_to_disk(fe.name, data_ptr, fe.meta.orig_size, fe.meta.timestamp);
            result.bytes_out += fe.meta.orig_size;
        }
        
        block_pos += fe.meta.orig_size;
    }

    fclose(f);
    result.ok = true;
    return result;
}

TarcResult list(const std::string& arch_path) {
    TarcResult res;
    FILE* f = fopen(arch_path.c_str(), "rb");
    if (!f) return {false, "Errore apertura archivio"};
    ::Header h; 
    fread(&h, sizeof(h), 1, f);
    std::vector<::FileEntry> toc;
    if (!IO::read_toc(f, h, toc)) { fclose(f); return {false, "TOC corrotto"}; }
    
    std::cout << "\n--- ARCHIVIO SOLID (.strk) ---\n";
    uint64_t total_size = 0;
    for (const auto& fe : toc) {
        uint64_t status_flag = fe.meta.is_duplicate ? 0 : 1;
        UI::print_list_entry(fe.name, fe.meta.orig_size, status_flag, (Codec)fe.meta.codec);
        if (!fe.meta.is_duplicate) total_size += fe.meta.orig_size;
    }
    std::cout << "\nDimensione totale unica: " << (total_size / 1024) << " KB\n";
    fclose(f);
    res.ok = true;
    return res;
}

TarcResult remove_files(const std::string&, const std::vector<std::string>&) { 
    return {false, "Rimozione non disponibile in modalità Solid"}; 
}

} // namespace Engine
