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
#include <set>

#ifdef _WIN32
    #include <windows.h>
#endif

#include "lzma.h"
extern "C" {
    #include "xxhash.h"
}

namespace fs = std::filesystem;

namespace CodecSelector {
    bool is_compressible(const std::string& ext) {
        static const std::set<std::string> skip = { ".jpg", ".png", ".mp4", ".zip", ".7z", ".rar", ".gz", ".mp3", ".pdf" };
        std::string e = ext;
        std::transform(e.begin(), e.end(), e.begin(), ::tolower);
        return skip.find(e) == skip.end();
    }
    Codec select(const std::string& path, size_t size) {
        if (!is_compressible(fs::path(path).extension().string())) return Codec::STORE;
        return (size < 1024 * 512) ? Codec::ZSTD : Codec::LZMA;
    }
}

namespace Engine {

std::string normalize_path(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}

void resolve_wildcards(const std::string& pattern, std::vector<std::string>& out) {
#ifdef _WIN32
    WIN32_FIND_DATAA findData;
    HANDLE hFind = FindFirstFileA(pattern.c_str(), &findData);
    if (hFind == INVALID_HANDLE_VALUE) return;
    std::string directory = "";
    size_t last_slash = pattern.find_last_of("\\/");
    if (last_slash != std::string::npos) directory = pattern.substr(0, last_slash + 1);
    do {
        std::string foundName(findData.cFileName); 
        if (foundName != "." && foundName != "..") {
            std::string fullPath = directory + foundName;
            if (fs::exists(fullPath)) {
                if (fs::is_regular_file(fullPath)) out.push_back(normalize_path(fullPath));
                else if (fs::is_directory(fullPath)) {
                    for (auto& p : fs::recursive_directory_iterator(fullPath))
                        if (p.is_regular_file()) out.push_back(normalize_path(p.path().string()));
                }
            }
        }
    } while (FindNextFileA(hFind, &findData));
    FindClose(hFind);
#else
    if (fs::exists(pattern)) {
        if (fs::is_directory(pattern)) {
            for (auto& p : fs::recursive_directory_iterator(pattern)) 
                if (p.is_regular_file()) out.push_back(normalize_path(p.path().string()));
        } else out.push_back(normalize_path(pattern));
    }
#endif
}

struct ChunkResult {
    std::vector<char> compressed_data;
    uint32_t raw_size;
    Codec codec;
    bool success;
};

ChunkResult compress_worker(std::vector<char> raw_data, int level, Codec chosen_codec) {
    ChunkResult res;
    res.raw_size = (uint32_t)raw_data.size();
    res.codec = chosen_codec;
    res.success = false;
    if (chosen_codec == Codec::STORE) {
        res.compressed_data = std::move(raw_data);
        res.success = true;
    } else {
        size_t max_out = lzma_stream_buffer_bound(raw_data.size());
        res.compressed_data.resize(max_out);
        size_t out_pos = 0;
        uint32_t preset = (level < 0) ? 9 : (uint32_t)level;
        lzma_ret ret = lzma_easy_buffer_encode(preset | LZMA_PRESET_EXTREME, LZMA_CHECK_CRC64, NULL,
            (const uint8_t*)raw_data.data(), raw_data.size(),
            (uint8_t*)res.compressed_data.data(), &out_pos, max_out);
        if (ret == LZMA_OK) { res.compressed_data.resize(out_pos); res.success = true; }
    }
    return res;
}

TarcResult compress(const std::string& archive_path, const std::vector<std::string>& inputs, bool append, int level) {
    TarcResult result;
    std::vector<std::string> expanded_files;
    for (const auto& in : inputs) resolve_wildcards(in, expanded_files);
    if (expanded_files.empty()) return {false, "Nessun file trovato."};

    std::vector<::FileEntry> final_toc;
    std::map<uint64_t, uint32_t> hash_map;
    ::Header h{};

    // --- LOGICA APPEND ---
    if (append && fs::exists(archive_path)) {
        FILE* f_old = fopen(archive_path.c_str(), "rb");
        if (f_old) {
            fread(&h, sizeof(h), 1, f_old);
            IO::read_toc(f_old, h, final_toc);
            fclose(f_old);
            for (size_t k = 0; k < final_toc.size(); ++k) {
                if (!final_toc[k].meta.is_duplicate) hash_map[final_toc[k].meta.xxhash] = (uint32_t)k;
            }
        }
    } else {
        memcpy(h.magic, TARC_MAGIC, 4);
        h.version = TARC_VERSION;
    }

    FILE* f = fopen(archive_path.c_str(), append ? "rb+" : "wb");
    if (!f) return {false, "Errore apertura archivio. Verifica permessi o antivirus."};

    if (append) fseek(f, (long)h.toc_offset, SEEK_SET);
    else fwrite(&h, sizeof(h), 1, f);

    std::vector<char> solid_buf;
    // Chunk ridotto a 32MB per stabilità su Windows Server
    size_t CHUNK_THRESHOLD = 32 * 1024 * 1024;
    std::future<ChunkResult> future_chunk;
    bool worker_active = false;

    auto write_worker = [&](std::future<ChunkResult>& fut) -> bool {
        ChunkResult res = fut.get();
        if (!res.success) return false;
        ::ChunkHeader ch = { (uint32_t)res.codec, res.raw_size, (uint32_t)res.compressed_data.size(), 0 };
        fwrite(&ch, sizeof(ch), 1, f);
        fwrite(res.compressed_data.data(), 1, res.compressed_data.size(), f);
        result.bytes_out += res.compressed_data.size();
        return true;
    };

    for (size_t i = 0; i < expanded_files.size(); ++i) {
        std::string disk_path = expanded_files[i];
        UI::print_progress((uint32_t)i + 1, (uint32_t)expanded_files.size(), fs::path(disk_path).filename().string());

        if (!fs::exists(disk_path)) continue;
        uintmax_t fsize = fs::file_size(disk_path);
        std::vector<char> data(fsize);

        bool read_ok = false;
#ifdef _WIN32
        // Lettura con Share Mode abilitata per file bloccati (Database)
        HANDLE hFile = CreateFileA(disk_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            DWORD bytesRead;
            if (ReadFile(hFile, data.data(), (DWORD)fsize, &bytesRead, NULL) && bytesRead == (DWORD)fsize) read_ok = true;
            else {
                // Diagnostica per Windows Server
                UI::print_error("Errore lettura: " + fs::path(disk_path).filename().string() + " (WinErr: " + std::to_string(GetLastError()) + ")");
            }
            CloseHandle(hFile);
        } else {
            UI::print_error("Apertura negata: " + fs::path(disk_path).filename().string() + " (WinErr: " + std::to_string(GetLastError()) + ")");
        }
#else
        FILE* in_f = fopen(disk_path.c_str(), "rb");
        if (in_f) {
            if (fread(data.data(), 1, fsize, in_f) == fsize) read_ok = true;
            fclose(in_f);
        }
#endif

        if (!read_ok) continue;

        uint64_t h64 = XXH64(data.data(), fsize, 0);
        ::FileEntry fe;
        fe.name = normalize_path(disk_path);
        fe.meta.orig_size = fsize;
        fe.meta.xxhash = h64;
        fe.meta.codec = (uint8_t)CodecSelector::select(disk_path, fsize);
        fe.meta.timestamp = (uint64_t)std::chrono::duration_cast<std::chrono::seconds>(fs::last_write_time(disk_path).time_since_epoch()).count();

        if (hash_map.count(h64)) {
            fe.meta.is_duplicate = 1;
            fe.meta.duplicate_of_idx = hash_map[h64];
        } else {
            hash_map[h64] = (uint32_t)final_toc.size();
            fe.meta.is_duplicate = 0;
            if (!solid_buf.empty() && (solid_buf.size() + fsize > CHUNK_THRESHOLD)) {
                if (worker_active && !write_worker(future_chunk)) return {false, "Errore compressione solid."};
                future_chunk = std::async(std::launch::async, compress_worker, std::move(solid_buf), level, (Codec)fe.meta.codec);
                worker_active = true;
                solid_buf.clear();
            }
            solid_buf.insert(solid_buf.end(), data.begin(), data.end());
            result.bytes_in += fsize;
        }
        final_toc.push_back(fe);
    }

    if (worker_active && !write_worker(future_chunk)) return {false, "Errore chunk finale."};
    if (!solid_buf.empty()) {
        ChunkResult last = compress_worker(std::move(solid_buf), level, Codec::LZMA);
        ::ChunkHeader ch = { (uint32_t)last.codec, last.raw_size, (uint32_t)last.compressed_data.size(), 0 };
        fwrite(&ch, sizeof(ch), 1, f);
        fwrite(last.compressed_data.data(), 1, last.compressed_data.size(), f);
    }

    ::ChunkHeader end_mark = {0, 0, 0, 0};
    fwrite(&end_mark, sizeof(end_mark), 1, f);
    IO::write_toc(f, h, final_toc);
    fclose(f);
    result.ok = true;
    return result;
}

TarcResult extract(const std::string& arch_path, const std::vector<std::string>& patterns, bool test_only) {
    TarcResult result;
    FILE* f = fopen(arch_path.c_str(), "rb");
    if (!f) return {false, "Archivio non trovato."};
    ::Header h; fread(&h, sizeof(h), 1, f);
    std::vector<::FileEntry> toc;
    IO::read_toc(f, h, toc);
    fseek(f, sizeof(::Header), SEEK_SET);
    std::vector<char> current_block;
    size_t block_pos = 0;
    for (size_t i = 0; i < toc.size(); ++i) {
        auto& fe = toc[i];
        if (fe.meta.is_duplicate) continue;
        if (block_pos >= current_block.size()) {
            ::ChunkHeader ch;
            if (fread(&ch, sizeof(ch), 1, f) != 1 || ch.raw_size == 0) break;
            std::vector<char> comp(ch.comp_size);
            fread(comp.data(), 1, ch.comp_size, f);
            current_block.assign(ch.raw_size, 0);
            if (ch.codec == (uint32_t)Codec::LZMA) {
                size_t src_p = 0, dst_p = 0; uint64_t limit = UINT64_MAX;
                lzma_stream_buffer_decode(&limit, 0, NULL, (const uint8_t*)comp.data(), &src_p, ch.comp_size, (uint8_t*)current_block.data(), &dst_p, ch.raw_size);
            } else memcpy(current_block.data(), comp.data(), ch.raw_size);
            block_pos = 0;
        }
        UI::print_progress((uint32_t)i + 1, (uint32_t)toc.size(), fe.name);
        if (!test_only) IO::write_file_to_disk(fe.name, current_block.data() + block_pos, fe.meta.orig_size, fe.meta.timestamp);
        result.bytes_out += fe.meta.orig_size;
        block_pos += fe.meta.orig_size;
    }
    fclose(f);
    result.ok = true; return result;
}

TarcResult list(const std::string& arch_path) {
    TarcResult res;
    FILE* f = fopen(arch_path.c_str(), "rb");
    if (!f) return {false, "Errore"};
    ::Header h; fread(&h, sizeof(h), 1, f);
    std::vector<::FileEntry> toc;
    IO::read_toc(f, h, toc);
    for (const auto& fe : toc) UI::print_list_entry(fe.name, fe.meta.orig_size, fe.meta.is_duplicate ? 0 : 1, (Codec)fe.meta.codec);
    fclose(f);
    res.ok = true; return res;
}

TarcResult remove_files(const std::string&, const std::vector<std::string>&) { return {false, "N/A"}; }

}
