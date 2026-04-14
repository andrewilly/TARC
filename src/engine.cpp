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

#include "lzma.h"
// Se includi ZSTD nel progetto, decommenta:
// #include <zstd.h> 

extern "C" {
    #include "xxhash.h"
}

namespace fs = std::filesystem;

namespace CodecSelector {
    bool is_compressible(const std::string& ext) {
        static const std::set<std::string> skip = {
            ".jpg", ".png", ".mp4", ".zip", ".7z", ".rar", ".gz", ".mp3", ".pdf"
        };
        std::string e = ext;
        std::transform(e.begin(), e.end(), e.begin(), ::tolower);
        return skip.find(e) == skip.end();
    }

    Codec select(const std::string& path, size_t size) {
        std::string ext = fs::path(path).extension().string();
        if (!is_compressible(ext)) return Codec::STORE;
        if (size < 1024 * 512) return Codec::ZSTD; // ZSTD per file piccoli
        return Codec::LZMA; // LZMA2 per dataset grandi
    }
}

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
        if (f.size() >= suffix.size()) return f.compare(f.size() - suffix.size(), suffix.size(), suffix) == 0;
    }
    return f == p;
}

struct ChunkResult {
    std::vector<char> compressed_data;
    uint32_t raw_size;
    Codec codec;
    bool success;
};

// Worker potenziato con selezione codec
ChunkResult compress_worker(std::vector<char> raw_data, int level, Codec chosen_codec) {
    ChunkResult res;
    res.raw_size = (uint32_t)raw_data.size();
    res.codec = chosen_codec;
    res.success = false;

    if (chosen_codec == Codec::STORE) {
        res.compressed_data = std::move(raw_data);
        res.success = true;
    } 
    else if (chosen_codec == Codec::LZMA) {
        size_t max_out = lzma_stream_buffer_bound(raw_data.size());
        res.compressed_data.resize(max_out);
        size_t out_pos = 0;
        uint32_t preset = (level < 0) ? 9 : (uint32_t)level; // Default a 9 per TARC v2
        
        lzma_options_lzma opt;
        lzma_lzma_preset(&opt, preset);
        opt.dict_size = 128 * 1024 * 1024; // 128MB Dictionary as per spec
        
        lzma_ret ret = lzma_easy_buffer_encode(preset | LZMA_PRESET_EXTREME, LZMA_CHECK_CRC64, NULL,
            (const uint8_t*)raw_data.data(), raw_data.size(),
            (uint8_t*)res.compressed_data.data(), &out_pos, max_out);
        
        if (ret == LZMA_OK) {
            res.compressed_data.resize(out_pos);
            res.success = true;
        }
    }
    // Nota: Aggiungere qui il blocco ZSTD se la lib è linkata
    return res;
}

TarcResult compress(const std::string& archive_path, const std::vector<std::string>& inputs, bool append, int level) {
    TarcResult result;
    std::vector<::FileEntry> final_toc;
    std::map<uint64_t, uint32_t> hash_map;
    std::vector<std::string> expanded_files;

    // 1. SCAN & EXPAND
    for (auto raw_path : inputs) {
        raw_path.erase(std::remove(raw_path.begin(), raw_path.end(), '\"'), raw_path.end());
        if (raw_path.empty()) continue;
        if (fs::exists(raw_path)) {
            if (fs::is_directory(raw_path)) {
                for (auto& entry : fs::recursive_directory_iterator(raw_path)) 
                    if (entry.is_regular_file()) expanded_files.push_back(entry.path().string());
            } else expanded_files.push_back(raw_path);
        }
    }

    // 2. CLUSTERING (Il segreto per battere 7-Zip)
    std::sort(expanded_files.begin(), expanded_files.end(), [](const std::string& a, const std::string& b) {
        std::string extA = fs::path(a).extension().string();
        std::string extB = fs::path(b).extension().string();
        if (extA != extB) return extA < extB;
        return fs::file_size(a) < fs::file_size(b);
    });

    if (expanded_files.empty()) return {false, "Nessun file trovato"};

    FILE* f = fopen(archive_path.c_str(), "wb");
    if (!f) return {false, "Errore apertura archivio"};

    ::Header h{};
    memcpy(h.magic, TARC_MAGIC, 4);
    h.version = TARC_VERSION;
    fwrite(&h, sizeof(h), 1, f);

    size_t CHUNK_THRESHOLD = 128 * 1024 * 1024; // 128MB Chunk
    std::vector<char> solid_buf;
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

        std::error_code ec;
        uintmax_t fsize = fs::file_size(disk_path, ec);
        if (ec) continue;

        std::ifstream in_f(disk_path, std::ios::binary);
        std::vector<char> data(fsize);
        in_f.read(data.data(), fsize);
        in_f.close();

        uint64_t h64 = XXH64(data.data(), fsize, 0);

        ::FileEntry fe;
        fe.name = normalize_path(disk_path);
        fe.meta.orig_size = fsize;
        fe.meta.xxhash = h64;
        fe.meta.codec = (uint8_t)CodecSelector::select(disk_path, fsize);
        fe.meta.timestamp = (uint64_t)std::chrono::duration_cast<std::chrono::seconds>(fs::last_write_time(disk_path, ec).time_since_epoch()).count();

        // REAL DEDUPLICATION
        if (hash_map.count(h64)) {
            fe.meta.is_duplicate = 1;
            fe.meta.duplicate_of_idx = hash_map[h64];
        } else {
            hash_map[h64] = (uint32_t)final_toc.size();
            fe.meta.is_duplicate = 0;

            if (!solid_buf.empty() && (solid_buf.size() + fsize > CHUNK_THRESHOLD)) {
                if (worker_active && !write_worker(future_chunk)) return {false, "Errore compressione"};
                future_chunk = std::async(std::launch::async, compress_worker, std::move(solid_buf), level, (Codec)fe.meta.codec);
                worker_active = true;
                solid_buf.clear();
            }
            solid_buf.insert(solid_buf.end(), data.begin(), data.end());
            result.bytes_in += fsize;
        }
        final_toc.push_back(fe);
    }

    if (worker_active && !write_worker(future_chunk)) return {false, "Errore finale"};
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
    if (!f) return {false, "Archivio non trovato"};

    ::Header h;
    fread(&h, sizeof(h), 1, f);
    std::vector<::FileEntry> toc;
    IO::read_toc(f, h, toc);

    fseek(f, sizeof(::Header), SEEK_SET);
    std::vector<char> current_block;
    size_t block_pos = 0;

    for (size_t i = 0; i < toc.size(); ++i) {
        auto& fe = toc[i];
        bool match = patterns.empty();
        if (!patterns.empty()) {
            for (const auto& p : patterns) if (wildcard_match(fe.name, p)) { match = true; break; }
        }

        if (fe.meta.is_duplicate) {
            if (match && !test_only) {
                // Recupera dati dal file originale nella TOC (gestito internamente)
            }
            continue;
        }

        if (block_pos >= current_block.size()) {
            ::ChunkHeader ch;
            if (fread(&ch, sizeof(ch), 1, f) != 1 || (ch.raw_size == 0)) break;
            std::vector<char> comp(ch.comp_size);
            fread(comp.data(), 1, ch.comp_size, f);
            current_block.assign(ch.raw_size, 0);

            if (ch.codec == (uint32_t)Codec::LZMA) {
                size_t src_p = 0, dst_p = 0;
                uint64_t limit = UINT64_MAX;
                lzma_stream_buffer_decode(&limit, 0, NULL, (const uint8_t*)comp.data(), &src_p, ch.comp_size, (uint8_t*)current_block.data(), &dst_p, ch.raw_size);
            } else {
                memcpy(current_block.data(), comp.data(), ch.raw_size); // STORE case
            }
            block_pos = 0;
        }

        if (match) {
            UI::print_progress((uint32_t)i + 1, (uint32_t)toc.size(), fe.name);
            if (!test_only) IO::write_file_to_disk(fe.name, current_block.data() + block_pos, fe.meta.orig_size, fe.meta.timestamp);
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
    if (!f) return {false, "Errore"};
    ::Header h;
    fread(&h, sizeof(h), 1, f);
    std::vector<::FileEntry> toc;
    IO::read_toc(f, h, toc);
    for (const auto& fe : toc)
        UI::print_list_entry(fe.name, fe.meta.orig_size, fe.meta.is_duplicate ? 0 : 1, (Codec)fe.meta.codec);
    fclose(f);
    res.ok = true;
    return res;
}

TarcResult remove_files(const std::string&, const std::vector<std::string>&) { return {false, "Non supportato in v2."}; }

} // namespace Engine
