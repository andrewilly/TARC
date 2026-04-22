#include "engine.h"
#include "io.h"
#include "ui.h"
#include "types.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>
#include <iostream>
#include <chrono>
#include <algorithm>
#include <set>
#include <map>

#ifdef _WIN32
    #include <windows.h>
    #define fseek64 _fseeki64
#else
    #define fseek64 fseeko
#endif

#include "lzma.h"
extern "C" {
    #include "xxhash.h"
}

namespace fs = std::filesystem;

namespace CodecSelector {
    static std::set<std::string> skip_dedup_exts = { ".mdb", ".accdb", ".ldb", ".sdf" };
    
    bool should_skip_dedup(const std::string& path) {
        std::string ext = fs::path(path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        return skip_dedup_exts.find(ext) != skip_dedup_exts.end();
    }
    
    bool is_compressibile(const std::string& ext) {
        static const std::set<std::string> skip = { ".zip", ".7z", ".rar", ".gz", ".mp4", ".jpg", ".png", ".webp" };
        std::string e = ext;
        std::transform(e.begin(), e.end(), e.begin(), ::tolower);
        return skip.find(e) == skip.end();
    }
    
    Codec select(const std::string& path, size_t size) {
        fs::path p(path);
        if (!is_compressibile(p.extension().string())) return Codec::STORE;
        if (size < 512 * 1024) return Codec::ZSTD;
        return Codec::LZMA;
    }
    
    void set_skip_extensions(const std::vector<std::string>& exts) {
        skip_dedup_exts.clear();
        for (const auto& e : exts) {
            std::string lower = e;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            skip_dedup_exts.insert(lower);
        }
    }
}

namespace Engine {

static size_t g_chunk_threshold = 256 * 1024 * 1024;

void set_chunk_threshold(size_t threshold) {
    g_chunk_threshold = threshold;
    UI::print_info("Chunk threshold impostato a " + UI::human_size(threshold));
}

void set_compression_workers(size_t) {
    // Non utilizzato - versione semplificata senza thread pool
}

struct ChunkResult {
    std::vector<char> compressed_data;
    uint32_t raw_size = 0;
    Codec codec = Codec::STORE;
    bool success = false;
    uint64_t content_hash = 0;
};

static ChunkResult compress_sync(const std::vector<char>& raw_data, int level, Codec chosen_codec) {
    ChunkResult res;
    res.raw_size = static_cast<uint32_t>(raw_data.size());
    res.codec = chosen_codec;
    
    if (chosen_codec == Codec::STORE) {
        res.compressed_data = raw_data;
        res.success = true;
        res.content_hash = XXH64(res.compressed_data.data(), res.compressed_data.size(), 0);
    } else if (chosen_codec == Codec::LZMA) {
        size_t max_out = lzma_stream_buffer_bound(raw_data.size());
        res.compressed_data.resize(max_out);
        size_t out_pos = 0;
        uint32_t preset = (level < 0) ? 9 : static_cast<uint32_t>(level);
        
        lzma_ret ret = lzma_easy_buffer_encode(preset | LZMA_PRESET_EXTREME, LZMA_CHECK_CRC64, nullptr,
            reinterpret_cast<const uint8_t*>(raw_data.data()), raw_data.size(),
            reinterpret_cast<uint8_t*>(res.compressed_data.data()), &out_pos, max_out);
            
        if (ret == LZMA_OK) {
            res.compressed_data.resize(out_pos);
            res.success = true;
            res.content_hash = XXH64(res.compressed_data.data(), res.compressed_data.size(), 0);
        } else {
            UI::print_warning("LZMA compression failed, falling back to STORE");
            res.compressed_data = raw_data;
            res.codec = Codec::STORE;
            res.success = true;
            res.content_hash = XXH64(res.compressed_data.data(), res.compressed_data.size(), 0);
        }
    } else {
        res.compressed_data = raw_data;
        res.codec = Codec::STORE;
        res.success = true;
        res.content_hash = XXH64(res.compressed_data.data(), res.compressed_data.size(), 0);
    }
    
    return res;
}

TarcResult compress(const std::string& archive_path, const std::vector<std::string>& inputs, bool append, int level) {
    TarcResult result;
    
    std::vector<std::string> expanded_files;
    for (const auto& in : inputs) {
        IO::resolve_wildcards(in, expanded_files);
    }
    
    if (expanded_files.empty()) {
        return {false, "Nessun file trovato."};
    }
    
    UI::print_info("Preparazione compressione di " + std::to_string(expanded_files.size()) + " file...");
    
    std::vector<FileEntry> final_toc;
    ConcurrentHashMap hash_map;
    Header h{};
    std::memcpy(h.magic, TARC_MAGIC, 4);
    h.version = TARC_VERSION;
    
    if (append && fs::exists(archive_path)) {
        FILE* f_old = fopen(archive_path.c_str(), "rb");
        if (f_old) {
            std::fread(&h, sizeof(h), 1, f_old);
            IO::read_toc(f_old, h, final_toc);
            fclose(f_old);
            for (size_t k = 0; k < final_toc.size(); ++k) {
                if (!final_toc[k].meta.is_duplicate) {
                    fs::path p(final_toc[k].name);
                    hash_map.insert(final_toc[k].meta.xxhash, static_cast<uint32_t>(k), p.extension().string());
                }
            }
        }
    }
    
    FILE* f = fopen(archive_path.c_str(), append ? "rb+" : "wb");
    if (!f) {
        return {false, "Impossibile aprire l'archivio: " + std::string(strerror(errno))};
    }
    
    if (append) {
        fseek64(f, static_cast<off_t>(h.toc_offset), SEEK_SET);
    } else {
        fwrite(&h, sizeof(h), 1, f);
    }
    
    std::vector<char> solid_buf;
    solid_buf.reserve(g_chunk_threshold);
    
    for (size_t i = 0; i < expanded_files.size(); ++i) {
        const std::string& disk_path = expanded_files[i];
        UI::print_progress(i + 1, expanded_files.size(), fs::path(disk_path).filename().string());
        
        if (!fs::exists(disk_path)) {
            UI::print_warning("File non trovato: " + disk_path);
            continue;
        }
        
        uintmax_t fsize = fs::file_size(disk_path);
        
        std::vector<char> data;
        bool read_ok = false;
        uint64_t h64 = 0;
        
        try {
            data.resize(fsize);
        } catch (const std::bad_alloc&) {
            UI::print_error("Memoria insufficiente per: " + disk_path);
            continue;
        }
        
        XXH64_state_t* const state = XXH64_createState();
        if (state) XXH64_reset(state, 0);
        
#ifdef _WIN32
        HANDLE hFile = CreateFileA(disk_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (hFile != INVALID_HANDLE_VALUE) {
            DWORD bytesReadTotal = 0;
            DWORD bytesRead = 0;
            const DWORD BUF_STEP = 1024 * 1024;
            char* ptr = data.data();
            
            while (bytesReadTotal < static_cast<DWORD>(fsize)) {
                DWORD toRead = (static_cast<DWORD>(fsize) - bytesReadTotal > BUF_STEP) ? BUF_STEP : static_cast<DWORD>(fsize) - bytesReadTotal;
                if (ReadFile(hFile, ptr + bytesReadTotal, toRead, &bytesRead, nullptr)) {
                    if (state) XXH64_update(state, ptr + bytesReadTotal, bytesRead);
                    bytesReadTotal += bytesRead;
                } else {
                    break;
                }
            }
            if (bytesReadTotal == static_cast<DWORD>(fsize)) read_ok = true;
            CloseHandle(hFile);
        }
#else
        FILE* in_f = fopen(disk_path.c_str(), "rb");
        if (in_f) {
            size_t total_read = 0;
            char* ptr = data.data();
            while (total_read < fsize) {
                size_t bytes_read = fread(ptr + total_read, 1, fsize - total_read, in_f);
                if (bytes_read == 0) break;
                if (state) XXH64_update(state, ptr + total_read, bytes_read);
                total_read += bytes_read;
            }
            if (total_read == fsize) read_ok = true;
            fclose(in_f);
        }
#endif
        
        if (state) {
            h64 = XXH64_digest(state);
            XXH64_freeState(state);
        }
        
        if (!read_ok) {
            UI::print_error("Lettura fallita per: " + disk_path);
            continue;
        }
        
        FileEntry fe;
        fe.name = IO::normalize_path(disk_path);
        fe.meta.orig_size = fsize;
        fe.meta.xxhash = h64;
        fe.meta.codec = static_cast<uint8_t>(CodecSelector::select(disk_path, fsize));
        fe.meta.timestamp = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
            fs::last_write_time(disk_path).time_since_epoch()).count());
        
        bool skip_dedup = CodecSelector::should_skip_dedup(disk_path);
        fs::path p(disk_path);
        std::string ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        
        auto dup_idx = hash_map.get(h64);
        if (!skip_dedup && dup_idx.has_value()) {
            fe.meta.is_duplicate = 1;
            fe.meta.duplicate_of_idx = dup_idx.value();
        } else {
            if (!skip_dedup) hash_map.insert(h64, static_cast<uint32_t>(final_toc.size()), ext);
            fe.meta.is_duplicate = 0;
            
            // Gestione chunk
            if (solid_buf.size() + fsize > g_chunk_threshold && !solid_buf.empty()) {
                ChunkResult res = compress_sync(solid_buf, level, static_cast<Codec>(fe.meta.codec));
                ChunkHeader ch = {
                    static_cast<uint32_t>(res.codec),
                    res.raw_size,
                    static_cast<uint32_t>(res.compressed_data.size()),
                    res.content_hash
                };
                fwrite(&ch, sizeof(ch), 1, f);
                fwrite(res.compressed_data.data(), 1, res.compressed_data.size(), f);
                result.bytes_out += res.compressed_data.size();
                solid_buf.clear();
                solid_buf.reserve(g_chunk_threshold);
            }
            
            if (solid_buf.capacity() < solid_buf.size() + fsize) {
                size_t new_cap = std::max(solid_buf.capacity() * 2, solid_buf.size() + fsize);
                new_cap = std::min(new_cap, g_chunk_threshold * 2);
                try {
                    solid_buf.reserve(new_cap);
                } catch (const std::bad_alloc&) {
                    UI::print_error("Memoria esaurita");
                    solid_buf.clear();
                    solid_buf.reserve(g_chunk_threshold);
                }
            }
            
            solid_buf.insert(solid_buf.end(), data.begin(), data.end());
            result.bytes_in += fsize;
        }
        final_toc.push_back(std::move(fe));
    }
    
    // Chunk finale
    if (!solid_buf.empty()) {
        ChunkResult last = compress_sync(solid_buf, level, Codec::LZMA);
        ChunkHeader ch = {
            static_cast<uint32_t>(last.codec),
            last.raw_size,
            static_cast<uint32_t>(last.compressed_data.size()),
            last.content_hash
        };
        fwrite(&ch, sizeof(ch), 1, f);
        fwrite(last.compressed_data.data(), 1, last.compressed_data.size(), f);
        result.bytes_out += last.compressed_data.size();
    }
    
    ChunkHeader end_mark = {0, 0, 0, 0};
    fwrite(&end_mark, sizeof(end_mark), 1, f);
    
    IO::write_toc(f, h, final_toc);
    fclose(f);
    
    result.ok = true;
    return result;
}

TarcResult extract(const std::string& arch_path, const std::vector<std::string>& patterns, bool test_only, size_t offset, bool flat_mode) {
    TarcResult result;
    
    FILE* f = fopen(arch_path.c_str(), "rb");
    if (!f) {
        return {false, "Archivio non trovato: " + arch_path};
    }
    
    if (offset > 0) {
        fseek64(f, static_cast<off_t>(offset), SEEK_SET);
    }
    
    Header h{};
    if (fread(&h, sizeof(h), 1, f) != 1) {
        fclose(f);
        return {false, "Header corrotto"};
    }
    
    if (std::memcmp(h.magic, TARC_MAGIC, 4) != 0) {
        fclose(f);
        return {false, "Magic number non valido"};
    }
    
    std::vector<FileEntry> toc;
    h.toc_offset += offset;
    
    if (!IO::read_toc(f, h, toc)) {
        fclose(f);
        return {false, "Impossibile leggere TOC"};
    }
    
    fseek64(f, static_cast<off_t>(offset + sizeof(Header)), SEEK_SET);
    
    std::vector<char> current_block;
    size_t block_pos = 0;
    std::map<std::string, int> flat_names_counter;
    
    size_t total_to_extract = 0;
    for (const auto& fe : toc) {
        if (patterns.empty()) {
            total_to_extract++;
        } else {
            for (const auto& pat : patterns) {
                if (IO::match_pattern(fe.name, pat)) {
                    total_to_extract++;
                    break;
                }
            }
        }
    }
    
    size_t extracted_count = 0;
    
    for (size_t i = 0; i < toc.size(); ++i) {
        const auto& fe = toc[i];
        
        bool should_extract = patterns.empty();
        if (!should_extract) {
            for (const auto& pat : patterns) {
                if (IO::match_pattern(fe.name, pat)) {
                    should_extract = true;
                    break;
                }
            }
        }
        
        if (!should_extract) {
            if (!fe.meta.is_duplicate) {
                if (block_pos >= current_block.size()) {
                    ChunkHeader ch;
                    if (fread(&ch, sizeof(ch), 1, f) != 1 || ch.raw_size == 0) break;
                    
                    std::vector<char> comp(ch.comp_size);
                    if (fread(comp.data(), 1, ch.comp_size, f) != ch.comp_size) break;
                    
                    current_block.resize(ch.raw_size);
                    
                    if (ch.codec == static_cast<uint32_t>(Codec::LZMA)) {
                        size_t src_p = 0, dst_p = 0;
                        uint64_t limit = UINT64_MAX;
                        lzma_stream_buffer_decode(&limit, 0, nullptr,
                            reinterpret_cast<const uint8_t*>(comp.data()), &src_p, ch.comp_size,
                            reinterpret_cast<uint8_t*>(current_block.data()), &dst_p, ch.raw_size);
                    } else {
                        std::memcpy(current_block.data(), comp.data(), ch.raw_size);
                    }
                    block_pos = 0;
                }
                block_pos += fe.meta.orig_size;
            }
            continue;
        }
        
        if (fe.meta.is_duplicate) {
            UI::print_extract(fe.name, fe.meta.orig_size, test_only, true);
            result.bytes_out += fe.meta.orig_size;
            extracted_count++;
            continue;
        }
        
        if (block_pos >= current_block.size()) {
            ChunkHeader ch;
            if (fread(&ch, sizeof(ch), 1, f) != 1 || ch.raw_size == 0) {
                UI::print_error("Fine archivio inaspettata");
                break;
            }
            
            std::vector<char> comp(ch.comp_size);
            if (fread(comp.data(), 1, ch.comp_size, f) != ch.comp_size) {
                fclose(f);
                return {false, "Errore lettura chunk"};
            }
            
            if (ch.content_hash != 0) {
                uint64_t computed = XXH64(comp.data(), ch.comp_size, 0);
                if (computed != ch.content_hash) {
                    UI::print_warning("Hash mismatch nel chunk");
                }
            }
            
            current_block.resize(ch.raw_size);
            
            if (ch.codec == static_cast<uint32_t>(Codec::LZMA)) {
                size_t src_p = 0, dst_p = 0;
                uint64_t limit = UINT64_MAX;
                lzma_ret ret = lzma_stream_buffer_decode(&limit, 0, nullptr,
                    reinterpret_cast<const uint8_t*>(comp.data()), &src_p, ch.comp_size,
                    reinterpret_cast<uint8_t*>(current_block.data()), &dst_p, ch.raw_size);
                    
                if (ret != LZMA_OK && ret != LZMA_STREAM_END) {
                    fclose(f);
                    return {false, "Errore decompressione LZMA"};
                }
            } else {
                std::memcpy(current_block.data(), comp.data(), ch.raw_size);
            }
            block_pos = 0;
        }
        
        UI::print_progress(extracted_count + 1, total_to_extract, fe.name);
        
        std::string final_path = fe.name;
        if (flat_mode) {
            fs::path p(fe.name);
            std::string filename = p.filename().string();
            
            auto it = flat_names_counter.find(filename);
            if (it != flat_names_counter.end()) {
                it->second++;
                size_t dot_pos = filename.find_last_of('.');
                if (dot_pos != std::string::npos) {
                    filename = filename.substr(0, dot_pos) + "_" + std::to_string(it->second) + filename.substr(dot_pos);
                } else {
                    filename += "_" + std::to_string(it->second);
                }
            } else {
                flat_names_counter[filename] = 0;
            }
            final_path = filename;
        }
        
        if (!test_only) {
            IO::write_file_to_disk(final_path, current_block.data() + block_pos, fe.meta.orig_size, fe.meta.timestamp);
        }
        
        UI::print_extract(final_path, fe.meta.orig_size, test_only, true);
        result.bytes_out += fe.meta.orig_size;
        extracted_count++;
        block_pos += fe.meta.orig_size;
    }
    
    fclose(f);
    result.ok = true;
    return result;
}

TarcResult list(const std::string& arch_path, size_t offset) {
    FILE* f = fopen(arch_path.c_str(), "rb");
    if (!f) {
        return {false, "Archivio non trovato"};
    }
    
    if (offset > 0) {
        fseek64(f, static_cast<off_t>(offset), SEEK_SET);
    }
    
    Header h{};
    if (fread(&h, sizeof(h), 1, f) != 1) {
        fclose(f);
        return {false, "Header corrotto"};
    }
    
    std::vector<FileEntry> toc;
    h.toc_offset += offset;
    
    if (!IO::read_toc(f, h, toc)) {
        fclose(f);
        return {false, "TOC corrotto"};
    }
    
    printf("\n%s=== %s ===%s\n", Color::CYAN, fs::path(arch_path).filename().string().c_str(), Color::RESET);
    printf("%sTARC v%d | %zu file%s\n\n", Color::DIM, h.version, toc.size(), Color::RESET);
    
    uint64_t total_size = 0;
    for (const auto& fe : toc) {
        UI::print_list_entry(fe.name, fe.meta.orig_size, fe.meta.is_duplicate ? 0 : 1, static_cast<Codec>(fe.meta.codec));
        total_size += fe.meta.orig_size;
    }
    
    printf("\n%sTotale: %s%s\n", Color::BOLD, UI::human_size(total_size).c_str(), Color::RESET);
    
    fclose(f);
    return {true, ""};
}

TarcResult create_sfx(const std::string& archive_path, const std::string& sfx_name) {
    std::string stub_path = "tarc_sfx_stub.exe";
    if (!fs::exists(stub_path)) {
#ifdef _WIN32
        char module_path[MAX_PATH];
        GetModuleFileNameA(nullptr, module_path, MAX_PATH);
        fs::path exe_dir = fs::path(module_path).parent_path();
        stub_path = (exe_dir / "tarc_sfx_stub.exe").string();
        if (!fs::exists(stub_path)) {
            return {false, "Stub SFX non trovato"};
        }
#else
        return {false, "Stub SFX non trovato (solo Windows)"};
#endif
    }
    
    std::ifstream stub_in(stub_path, std::ios::binary);
    std::ifstream data_in(archive_path, std::ios::binary);
    std::ofstream sfx_out(sfx_name, std::ios::binary);
    
    if (!stub_in || !data_in || !sfx_out) {
        return {false, "Errore apertura file per SFX"};
    }
    
    sfx_out << stub_in.rdbuf();
    sfx_out << data_in.rdbuf();
    
    return {true, "SFX creato: " + sfx_name};
}

TarcResult remove_files(const std::string&, const std::vector<std::string>&) {
    return {false, "Rimozione non supportata in modalità solid"};
}

} // namespace Engine
