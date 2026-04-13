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
#include <algorithm>

#ifdef _WIN32
    #include <windows.h>
#endif

#include "lzma.h"
extern "C" {
    #include "xxhash.h"
}

namespace fs = std::filesystem;

namespace Engine {

std::string normalize_path(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}

bool wildcard_match(const std::string& filename, const std::string& pattern) {
    if (pattern == "*" || pattern == "*.*") return true;
    std::string f = filename;
    std::string p = pattern;
    std::transform(f.begin(), f.end(), f.begin(), ::tolower);
    std::transform(p.begin(), p.end(), p.begin(), ::tolower);

    if (p.substr(0, 1) == "*") {
        std::string suffix = p.substr(1);
        if (f.size() >= suffix.size()) {
            return f.compare(f.size() - suffix.size(), suffix.size(), suffix) == 0;
        }
    }
    return f == p;
}

struct ChunkResult {
    std::vector<char> compressed_data;
    uint32_t raw_size;
    bool success;
};

ChunkResult compress_worker(std::vector<char> raw_data, int level) {
    ChunkResult res;
    res.raw_size = (uint32_t)raw_data.size();
    size_t max_out = lzma_stream_buffer_bound(raw_data.size());
    res.compressed_data.resize(max_out);
    size_t out_pos = 0;
    uint32_t preset = (level < 0) ? 6 : (uint32_t)level;
    
    lzma_ret ret = lzma_easy_buffer_encode(preset, LZMA_CHECK_CRC64, NULL, 
                                          (const uint8_t*)raw_data.data(), raw_data.size(), 
                                          (uint8_t*)res.compressed_data.data(), &out_pos, max_out);
    
    res.success = (ret == LZMA_OK);
    if (res.success) res.compressed_data.resize(out_pos);
    return res;
}

TarcResult compress(const std::string& archive_path, const std::vector<std::string>& inputs, bool append, int level) {
    TarcResult result;
    std::vector<::FileEntry> final_toc;
    std::map<uint64_t, uint32_t> hash_map;
    std::vector<std::string> expanded_files;

    // --- SCANSIONE ROBUSTA v1.12 ---
    for (const auto& in : inputs) {
        std::string path_str = in;
        if (path_str.empty()) continue;

        size_t last_slash = path_str.find_last_of("\\/");
        std::string folder = (last_slash == std::string::npos) ? "." : path_str.substr(0, last_slash);
        std::string pattern = (last_slash == std::string::npos) ? path_str : path_str.substr(last_slash + 1);

        if (pattern.find('*') != std::string::npos) {
            if (fs::exists(folder) && fs::is_directory(folder)) {
                for (auto& entry : fs::directory_iterator(folder)) {
                    if (entry.is_regular_file() && wildcard_match(entry.path().filename().string(), pattern)) {
                        expanded_files.push_back(entry.path().string());
                    }
                }
            }
        } else if (fs::is_directory(path_str)) {
            for (auto& entry : fs::recursive_directory_iterator(path_str)) {
                if (entry.is_regular_file()) expanded_files.push_back(entry.path().string());
            }
        } else if (fs::exists(path_str)) {
            expanded_files.push_back(path_str);
        }
    }

    if (expanded_files.empty()) return {false, "Nessun file trovato"};

    // Nota: Se append è true, qui andrebbe caricato il TOC esistente. 
    // Per ora creiamo un nuovo archivio per stabilità.
    FILE* f = fopen(archive_path.c_str(), "wb");
    if (!f) return {false, "Errore apertura file"};

    ::Header h;
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, "STRK", 4);
    h.version = 110;
    fwrite(&h, sizeof(h), 1, f);

    size_t CHUNK_THRESHOLD = 64 * 1024 * 1024;
    std::vector<char> solid_buf;
    std::future<ChunkResult> future_chunk;
    bool worker_active = false;

    for (size_t i = 0; i < expanded_files.size(); ++i) {
        std::string disk_path = expanded_files[i];
        UI::print_progress((uint32_t)i + 1, (uint32_t)expanded_files.size(), fs::path(disk_path).filename().string());
        
        uintmax_t fsize = fs::file_size(disk_path);
        std::ifstream in_f(disk_path, std::ios::binary);
        std::vector<char> data(fsize);
        in_f.read(data.data(), fsize);

        uint64_t h64 = XXH64(data.data(), fsize, 0);
        ::FileEntry fe;
        fe.name = normalize_path(disk_path);
        fe.meta.orig_size = (uint32_t)fsize;
        fe.meta.xxhash = h64;
        fe.meta.codec = (uint8_t)Codec::LZMA;

        if (hash_map.count(h64)) {
            fe.meta.is_duplicate = 1;
            fe.meta.duplicate_of_idx = hash_map[h64];
        } else {
            hash_map[h64] = (uint32_t)final_toc.size();
            if (worker_active && (solid_buf.size() + fsize > CHUNK_THRESHOLD)) {
                ChunkResult res = future_chunk.get();
                ::ChunkHeader ch = { res.raw_size, (uint32_t)res.compressed_data.size() };
                fwrite(&ch, sizeof(ch), 1, f);
                fwrite(res.compressed_data.data(), 1, res.compressed_data.size(), f);
                worker_active = false;
            }
            solid_buf.insert(solid_buf.end(), data.begin(), data.end());
            if (!worker_active) {
                future_chunk = std::async(std::launch::async, compress_worker, solid_buf, level);
                solid_buf.clear();
                worker_active = true;
            }
        }
        final_toc.push_back(fe);
    }

    if (worker_active) {
        ChunkResult res = future_chunk.get();
        ::ChunkHeader ch = { res.raw_size, (uint32_t)res.compressed_data.size() };
        fwrite(&ch, sizeof(ch), 1, f);
        fwrite(res.compressed_data.data(), 1, res.compressed_data.size(), f);
    }

    h.toc_offset = ftell(f);
    IO::write_toc(f, h, final_toc);
    fseek(f, 0, SEEK_SET);
    fwrite(&h, sizeof(h), 1, f);
    fclose(f);

    result.ok = true;
    return result;
}

TarcResult extract(const std::string& arch_path, const std::vector<std::string>& patterns, bool test_only) {
    TarcResult result;
    FILE* f = fopen(arch_path.c_str(), "rb");
    if (!f) return {false, "Impossibile aprire archivio"};

    ::Header h;
    if (fread(&h, sizeof(h), 1, f) != 1) { fclose(f); return {false, "Header corrotto"}; }
    std::vector<::FileEntry> toc;
    if (!IO::read_toc(f, h, toc)) { fclose(f); return {false, "TOC invalido"}; }

    fseek(f, sizeof(::Header), SEEK_SET);
    std::vector<char> current_block;
    size_t block_pos = 0;
    bool block_valid = false;

    for (size_t i = 0; i < toc.size(); ++i) {
        auto& fe = toc[i];
        bool match = patterns.empty();
        if (!patterns.empty()) {
            for (const auto& p : patterns) { if (wildcard_match(fe.name, p)) { match = true; break; } }
        }

        if (fe.meta.is_duplicate) {
            if (match && !test_only) {
                if (fs::exists(toc[fe.meta.duplicate_of_idx].name)) {
                    try { 
                        fs::path dest(fe.name);
                        if (dest.has_parent_path()) fs::create_directories(dest.parent_path());
                        fs::copy(toc[fe.meta.duplicate_of_idx].name, fe.name, fs::copy_options::overwrite_existing); 
                    } catch(...) {}
                }
            }
            if (match) result.bytes_out += fe.meta.orig_size;
            continue;
        }

        if (!block_valid || block_pos >= current_block.size()) {
            ::ChunkHeader ch;
            if (fread(&ch, sizeof(ch), 1, f) != 1 || (ch.raw_size == 0 && ch.comp_size == 0)) break;
            std::vector<char> comp(ch.comp_size);
            if(fread(comp.data(), 1, ch.comp_size, f) != ch.comp_size) break;
            
            current_block.assign(ch.raw_size, 0);
            size_t src_p = 0, dst_p = 0; uint64_t limit = UINT64_MAX;
            lzma_stream_buffer_decode(&limit, 0, NULL, (const uint8_t*)comp.data(), &src_p, ch.comp_size, (uint8_t*)current_block.data(), &dst_p, ch.raw_size);
            block_pos = 0; block_valid = true;
        }

        if (match) {
            UI::print_progress((uint32_t)i + 1, (uint32_t)toc.size(), fe.name);
            const char* ptr = current_block.data() + block_pos;
            if (XXH64(ptr, fe.meta.orig_size, 0) == fe.meta.xxhash) {
                if (!test_only) IO::write_file_to_disk(fe.name, ptr, fe.meta.orig_size, (time_t)fe.meta.timestamp);
                result.bytes_out += fe.meta.orig_size;
            }
        }
        block_pos += fe.meta.orig_size;
    }
    fclose(f);
    result.ok = true; return result;
}

TarcResult list(const std::string& arch_path) {
    TarcResult res;
    FILE* f = fopen(arch_path.c_str(), "rb");
    if (!f) return {false, "Errore apertura"};
    ::Header h; 
    if(fread(&h, sizeof(h), 1, f) != 1) { fclose(f); return {false, "Header non valido"}; }
    std::vector<::FileEntry> toc; 
    IO::read_toc(f, h, toc);
    std::cout << "\n--- ANALISI ARCHIVIO SOLID (.strk) ---\n";
    for (const auto& fe : toc) UI::print_list_entry(fe.name, fe.meta.orig_size, fe.meta.is_duplicate ? 0 : 1, (Codec)fe.meta.codec);
    fclose(f);
    res.ok = true; return res;
}

TarcResult remove_files(const std::string&, const std::vector<std::string>&) { return {false, "Non supportato in SOLID."}; }

} // namespace Engine
