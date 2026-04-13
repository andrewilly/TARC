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
#include <regex>
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

bool wildcard_match(const std::string& text, const std::string& pattern) {
    if (pattern == "*" || pattern == "*.*" || pattern.empty()) return true;
    try {
        std::string n_text = normalize_path(text);
        std::string n_pattern = normalize_path(pattern);
        if (n_pattern.back() == '/') return n_text.compare(0, n_pattern.size(), n_pattern) == 0;
        
        std::string r = n_pattern;
        r = std::regex_replace(r, std::regex("\\."), "\\."); 
        r = std::regex_replace(r, std::regex("\\*"), ".*");  
        r = std::regex_replace(r, std::regex("\\?"), ".");   
        std::regex re(r, std::regex_constants::icase);
        return std::regex_match(n_text, re) || n_text.compare(0, n_pattern.size(), n_pattern) == 0;
    } catch (...) { return false; }
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
    (void)append; 
    TarcResult result;
    std::vector<::FileEntry> final_toc;
    std::map<uint64_t, uint32_t> hash_map;
    std::vector<std::string> expanded_files;

    // Espansione input migliorata
    for (const auto& in : inputs) {
        if (in.find_first_of("*?") != std::string::npos) {
            fs::path p(in);
            fs::path dir = p.has_parent_path() ? p.parent_path() : ".";
            std::string pattern = p.filename().string();
            if (fs::exists(dir)) {
                for (auto& entry : fs::directory_iterator(dir)) {
                    if (wildcard_match(entry.path().filename().string(), pattern))
                        expanded_files.push_back(entry.path().string());
                }
            }
        } else if (fs::is_directory(in)) {
            for (auto& entry : fs::recursive_directory_iterator(in)) {
                if (entry.is_regular_file()) expanded_files.push_back(entry.path().string());
            }
        } else if (fs::exists(in)) {
            expanded_files.push_back(in);
        }
    }

    if (expanded_files.empty()) return {false, "Nessun file trovato"};

    std::string temp_path = archive_path + ".tmp";
    FILE* f = fopen(temp_path.c_str(), "wb");
    if (!f) return {false, "Errore file temporaneo"};

    ::Header h = {{'S','T','R','K'}, 110, 0, 0};
    fwrite(&h, sizeof(h), 1, f);

    size_t CHUNK_THRESHOLD = 64 * 1024 * 1024;
    std::vector<char> solid_buf;
    std::future<ChunkResult> future_chunk;
    bool worker_active = false;

    auto write_worker = [&](std::future<ChunkResult>& fut) {
        ChunkResult res = fut.get();
        if (res.success) {
            ::ChunkHeader ch = { res.raw_size, (uint32_t)res.compressed_data.size() };
            fwrite(&ch, sizeof(ch), 1, f);
            fwrite(res.compressed_data.data(), 1, res.compressed_data.size(), f);
            result.bytes_out += res.compressed_data.size();
        }
        return res.success;
    };

    for (size_t i = 0; i < expanded_files.size(); ++i) {
        std::string disk_path = expanded_files[i];
        if (!fs::is_regular_file(disk_path)) continue;

        UI::print_progress(i + 1, (uint32_t)expanded_files.size(), fs::path(disk_path).filename().string());
        
        uintmax_t fsize = fs::file_size(disk_path);
        std::ifstream in(disk_path, std::ios::binary);
        std::vector<char> data(fsize);
        if (fsize > 0) in.read(data.data(), fsize);
        in.close();

        uint64_t h64 = XXH64(data.data(), fsize, 0);
        ::FileEntry fe;
        fe.name = normalize_path(disk_path);
        fe.meta.orig_size = (uint32_t)fsize;
        fe.meta.xxhash = h64;
        fe.meta.codec = (uint8_t)Codec::LZMA;
        fe.meta.timestamp = std::chrono::duration_cast<std::chrono::seconds>(fs::last_write_time(disk_path).time_since_epoch()).count();

        if (hash_map.count(h64)) {
            fe.meta.is_duplicate = 1;
            fe.meta.duplicate_of_idx = hash_map[h64];
        } else {
            hash_map[h64] = (uint32_t)final_toc.size();
            fe.meta.is_duplicate = 0;
            if (!solid_buf.empty() && (solid_buf.size() + fsize > CHUNK_THRESHOLD)) {
                if (worker_active) write_worker(future_chunk);
                future_chunk = std::async(std::launch::async, compress_worker, std::move(solid_buf), level);
                worker_active = true;
                solid_buf.clear();
            }
            solid_buf.insert(solid_buf.end(), data.begin(), data.end());
            result.bytes_in += fsize;
        }
        final_toc.push_back(fe);
    }

    if (worker_active) write_worker(future_chunk);
    if (!solid_buf.empty()) {
        ChunkResult last = compress_worker(std::move(solid_buf), level);
        if (last.success) {
            ::ChunkHeader ch = { last.raw_size, (uint32_t)last.compressed_data.size() };
            fwrite(&ch, sizeof(ch), 1, f);
            fwrite(last.compressed_data.data(), 1, last.compressed_data.size(), f);
            result.bytes_out += last.compressed_data.size();
        }
    }

    ::ChunkHeader end_mark = {0, 0};
    fwrite(&end_mark, sizeof(end_mark), 1, f);
    uint64_t toc_pos = ftell(f);
    IO::write_toc(f, final_toc);
    
    h.toc_offset = toc_pos;
    h.num_files = (uint32_t)final_toc.size();
    fseek(f, 0, SEEK_SET);
    fwrite(&h, sizeof(h), 1, f);
    fclose(f);

    fs::rename(temp_path, archive_path);
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
            fread(comp.data(), 1, ch.comp_size, f);
            current_block.assign(ch.raw_size, 0);
            size_t src_p = 0, dst_p = 0; uint64_t limit = UINT64_MAX;
            lzma_stream_buffer_decode(&limit, 0, NULL, (uint8_t*)comp.data(), &src_p, ch.comp_size, (uint8_t*)current_block.data(), &dst_p, ch.raw_size);
            block_pos = 0; block_valid = true;
        }

        if (match) {
            UI::print_progress(i + 1, (uint32_t)toc.size(), fe.name);
            const char* ptr = current_block.data() + block_pos;
            if (XXH64(ptr, fe.meta.orig_size, 0) == fe.meta.xxhash) {
                if (!test_only) IO::write_file_to_disk(fe.name, ptr, fe.meta.orig_size, fe.meta.timestamp);
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
    ::Header h; fread(&h, sizeof(h), 1, f);
    std::vector<::FileEntry> toc; IO::read_toc(f, h, toc);
    std::cout << "\n--- ANALISI ARCHIVIO SOLID (.strk) ---\n";
    for (const auto& fe : toc) UI::print_list_entry(fe.name, fe.meta.orig_size, fe.meta.is_duplicate ? 0 : 1, (Codec)fe.meta.codec);
    fclose(f);
    res.ok = true; return res;
}

TarcResult remove_files(const std::string&, const std::vector<std::string>&) { return {false, "Non supportato in SOLID."}; }

} // namespace Engine
