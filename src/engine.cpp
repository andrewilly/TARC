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
#include <limits>

#ifdef _WIN32
    #include <windows.h>
#endif

#include "lzma.h"
extern "C" {
    #include "xxhash.h"
}

namespace fs = std::filesystem;

// Massimo numero di file nell'archivio (protezione da overflow)
constexpr size_t MAX_FILES_IN_ARCHIVE = 10'000'000;

// Limite chunk di default
constexpr size_t DEFAULT_CHUNK_THRESHOLD = 256 * 1024 * 1024;  // 256 MB

// ========== CLASSE ROLLBACK PER COMPRESSIONE ==========
class CompressionRollback {
private:
    FILE* file = nullptr;
    std::string archive_path;
    bool committed = false;
    long original_size = 0;
    
public:
    CompressionRollback(const std::string& path, FILE* f) 
        : file(f), archive_path(path) {
        if (file) {
            fseek(file, 0, SEEK_END);
            original_size = ftell(file);
        }
    }
    
    ~CompressionRollback() {
        if (!committed && file) {
            // Rollback: tronca il file alla dimensione originale
            fclose(file);
#ifdef _WIN32
            HANDLE h = CreateFileA(archive_path.c_str(), GENERIC_WRITE, 0, NULL,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                LARGE_INTEGER li;
                li.QuadPart = original_size;
                SetFilePointerEx(h, li, NULL, FILE_BEGIN);
                SetEndOfFile(h);
                CloseHandle(h);
            }
#else
            truncate(archive_path.c_str(), original_size);
#endif
        } else if (file) {
            fclose(file);
        }
    }
    
    void commit() { 
        committed = true; 
    }
    
    FILE* get_file() const { return file; }
};

// ========== FUNZIONI DI UTILITY ==========

// Funzione per disabilitare deduplicazione su file problematici
static bool should_skip_dedup(const std::string& path) {
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".mdb" || ext == ".accdb" || ext == ".ldb" || ext == ".sdf";
}

// Buffer di lettura adattivo in base alla dimensione del file
static size_t get_read_buffer_size(uintmax_t file_size) {
    if (file_size > 100 * 1024 * 1024) return 4 * 1024 * 1024;  // 4 MB per file > 100MB
    if (file_size > 10 * 1024 * 1024) return 2 * 1024 * 1024;   // 2 MB per file > 10MB
    return 1024 * 1024;  // 1 MB default
}

namespace CodecSelector {
    bool is_compressibile(const std::string& ext) {
        static const std::set<std::string> skip = { ".zip", ".7z", ".rar", ".gz", ".mp4", ".jpg", ".png", ".webp" };
        std::string e = ext;
        std::transform(e.begin(), e.end(), e.begin(), ::tolower);
        return skip.find(e) == skip.end();
    }
    
    Codec select(const std::string& path, size_t size) {
        if (!is_compressibile(fs::path(path).extension().string())) return Codec::STORE;
        return (size < 512 * 1024) ? Codec::ZSTD : Codec::LZMA;
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
    uint32_t raw_size = 0;
    Codec codec = Codec::STORE;
    bool success = false;
    uint64_t data_hash = 0;
};

ChunkResult compress_worker(std::vector<char> raw_data, int level, Codec chosen_codec) {
    ChunkResult res;
    res.raw_size = static_cast<uint32_t>(raw_data.size());
    res.codec = chosen_codec;
    res.success = false;
    
    if (chosen_codec == Codec::STORE) {
        res.compressed_data = std::move(raw_data);
        res.success = true;
        res.data_hash = XXH64(res.compressed_data.data(), res.compressed_data.size(), 0);
    } else {
        size_t max_out = lzma_stream_buffer_bound(raw_data.size());
        
        if (max_out == 0 || max_out > std::numeric_limits<size_t>::max() / 2) {
            UI::print_warning("Dimensione massima compressione non valida, fallback a STORE");
            res.compressed_data = std::move(raw_data);
            res.codec = Codec::STORE;
            res.success = true;
            res.data_hash = XXH64(res.compressed_data.data(), res.compressed_data.size(), 0);
            return res;
        }
        
        try {
            res.compressed_data.resize(max_out);
        } catch (const std::bad_alloc&) {
            UI::print_warning("Memoria insufficiente per compressione, fallback a STORE");
            res.compressed_data = std::move(raw_data);
            res.codec = Codec::STORE;
            res.success = true;
            res.data_hash = XXH64(res.compressed_data.data(), res.compressed_data.size(), 0);
            return res;
        }
        
        size_t out_pos = 0;
        uint32_t preset = (level < 0) ? 9 : static_cast<uint32_t>(level);
        
        lzma_ret ret = lzma_easy_buffer_encode(preset | LZMA_PRESET_EXTREME, LZMA_CHECK_CRC64, nullptr,
            reinterpret_cast<const uint8_t*>(raw_data.data()), raw_data.size(),
            reinterpret_cast<uint8_t*>(res.compressed_data.data()), &out_pos, max_out);
            
        if (ret == LZMA_OK) { 
            res.compressed_data.resize(out_pos); 
            res.success = true;
            res.data_hash = XXH64(res.compressed_data.data(), res.compressed_data.size(), 0);
        } else {
            const char* err_msg = "Unknown error";
            switch(ret) {
                case LZMA_MEM_ERROR: err_msg = "Memoria insufficiente"; break;
                case LZMA_OPTIONS_ERROR: err_msg = "Opzioni compressione non valide"; break;
                case LZMA_DATA_ERROR: err_msg = "Dati corrotti"; break;
                case LZMA_BUF_ERROR: err_msg = "Buffer insufficiente"; break;
                default: break;
            }
            UI::print_warning(std::string("LZMA compression failed (") + err_msg + "), falling back to STORE");
            res.compressed_data = std::move(raw_data);
            res.codec = Codec::STORE;
            res.success = true;
            res.data_hash = XXH64(res.compressed_data.data(), res.compressed_data.size(), 0);
        }
    }
    return res;
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
            return {false, "Stub SFX (tarc_sfx_stub.exe) non trovato."};
        }
#else
        return {false, "Stub SFX (tarc_sfx_stub.exe) non trovato (solo Windows)."};
#endif
    }

    std::ifstream stub_in(stub_path, std::ios::binary);
    std::ifstream data_in(archive_path, std::ios::binary);
    std::ofstream sfx_out(sfx_name, std::ios::binary);

    if (!stub_in || !data_in || !sfx_out) {
        return {false, "Errore fatale durante la fusione SFX."};
    }

    sfx_out << stub_in.rdbuf();
    sfx_out << data_in.rdbuf();
    
    return {true, "Archivio autoestraente generato."};
}

TarcResult compress(const std::string& archive_path, const std::vector<std::string>& inputs, bool append, int level) {
    TarcResult result;
    std::vector<std::string> expanded_files;
    
    for (const auto& in : inputs) {
        resolve_wildcards(in, expanded_files);
        if (expanded_files.size() > MAX_FILES_IN_ARCHIVE) {
            return {false, "Troppi file da archiviare. Massimo: " + std::to_string(MAX_FILES_IN_ARCHIVE)};
        }
    }
    
    if (expanded_files.empty()) {
        return {false, "Nessun file trovato."};
    }

    UI::print_info("Preparazione compressione di " + std::to_string(expanded_files.size()) + " file...");

    std::vector<::FileEntry> final_toc;
    std::map<uint64_t, uint32_t> hash_map;
    ::Header h{};
    std::memset(&h, 0, sizeof(h));

    if (append && fs::exists(archive_path)) {
        FILE* f_old = fopen(archive_path.c_str(), "rb");
        if (f_old) {
            if (fread(&h, sizeof(h), 1, f_old) != 1) {
                fclose(f_old);
                return {false, "Errore lettura header dell'archivio esistente."};
            }
            IO::read_toc(f_old, h, final_toc);
            fclose(f_old);
            
            if (final_toc.size() > MAX_FILES_IN_ARCHIVE) {
                return {false, "Archivio esistente ha troppi file. Massimo: " + std::to_string(MAX_FILES_IN_ARCHIVE)};
            }
            
            for (size_t k = 0; k < final_toc.size(); ++k) {
                if (!final_toc[k].meta.is_duplicate) {
                    hash_map[final_toc[k].meta.xxhash] = static_cast<uint32_t>(k);
                }
            }
        }
    } else {
        std::memcpy(h.magic, TARC_MAGIC, 4);
        h.version = TARC_VERSION;
    }

    FILE* f = fopen(archive_path.c_str(), append ? "rb+" : "wb");
    if (!f) {
        return {false, "ERRORE CRITICO: Impossibile scrivere l'archivio: " + std::string(strerror(errno))};
    }

    // ROLLBACK: se qualcosa va storto, l'archivio viene ripristinato
    CompressionRollback rollback(archive_path, f);

    if (append) {
        if (fseek(f, static_cast<long>(h.toc_offset), SEEK_SET) != 0) {
            return {false, "Errore seek in modalità append."};
        }
    } else {
        if (fwrite(&h, sizeof(h), 1, f) != 1) {
            return {false, "Errore scrittura header."};
        }
    }

    std::vector<char> solid_buf;
    size_t CHUNK_THRESHOLD = DEFAULT_CHUNK_THRESHOLD;
    
    if (expanded_files.size() < 10) {
        CHUNK_THRESHOLD = 32 * 1024 * 1024;
    }
    
    solid_buf.reserve(CHUNK_THRESHOLD);
    
    std::future<ChunkResult> future_chunk;
    bool worker_active = false;

    auto write_worker = [&](std::future<ChunkResult>& fut) -> bool {
        ChunkResult res = fut.get();
        if (!res.success) return false;
        
        ::ChunkHeader ch = { 
            static_cast<uint32_t>(res.codec), 
            res.raw_size, 
            static_cast<uint32_t>(res.compressed_data.size()),
            res.data_hash,
            0,  // crc32 (riservato per futuro)
            0   // reserved
        };
        
        if (fwrite(&ch, sizeof(ch), 1, f) != 1) return false;
        if (fwrite(res.compressed_data.data(), 1, res.compressed_data.size(), f) != res.compressed_data.size()) return false;
        
        result.bytes_out += res.compressed_data.size();
        return true;
    };

    for (size_t i = 0; i < expanded_files.size(); ++i) {
        const std::string& disk_path = expanded_files[i];
        UI::print_progress(i + 1, expanded_files.size(), fs::path(disk_path).filename().string());

        if (!fs::exists(disk_path)) {
            UI::print_warning("File non trovato: " + disk_path);
            continue;
        }
        
        std::error_code ec;
        uintmax_t fsize = fs::file_size(disk_path, ec);
        if (ec) {
            UI::print_warning("Impossibile ottenere dimensione: " + disk_path);
            continue;
        }
        
        std::vector<char> data;
        bool read_ok = false;
        uint64_t h64 = 0;
        
        if (fsize > static_cast<uintmax_t>(CHUNK_THRESHOLD) * 4) {
            UI::print_warning("File molto grande (" + UI::human_size(fsize) + "), potrebbe richiedere molta memoria");
        }
        
        try {
            data.resize(static_cast<size_t>(fsize));
        } catch (const std::bad_alloc&) {
            UI::print_error("Memoria insufficiente per: " + disk_path + " (" + UI::human_size(fsize) + ")");
            continue;
        }

        XXH64_state_t* const state = XXH64_createState();
        if (state) XXH64_reset(state, 0);

        size_t read_buffer_size = get_read_buffer_size(fsize);

#ifdef _WIN32
        HANDLE hFile = CreateFileA(disk_path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, 
                                    OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            DWORD bytesReadTotal = 0;
            DWORD bytesRead = 0;
            char* ptr = data.data();
            
            while (bytesReadTotal < static_cast<DWORD>(fsize)) {
                DWORD toRead = (static_cast<DWORD>(fsize) - bytesReadTotal > static_cast<DWORD>(read_buffer_size)) 
                                ? static_cast<DWORD>(read_buffer_size) : static_cast<DWORD>(fsize) - bytesReadTotal;
                if (ReadFile(hFile, ptr + bytesReadTotal, toRead, &bytesRead, NULL)) {
                    if (state) XXH64_update(state, ptr + bytesReadTotal, bytesRead);
                    bytesReadTotal += bytesRead;
                } else {
                    UI::print_error("Errore lettura: " + disk_path + " (WinErr: " + std::to_string(GetLastError()) + ")");
                    break;
                }
            }
            if (bytesReadTotal == static_cast<DWORD>(fsize)) read_ok = true;
            CloseHandle(hFile);
        } else {
            UI::print_error("Accesso negato: " + disk_path);
        }
#else
        FILE* in_f = fopen(disk_path.c_str(), "rb");
        if (in_f) {
            size_t total_read = 0;
            char* ptr = data.data();
            size_t bytes_read = 0;
            
            while (total_read < fsize) {
                size_t to_read = std::min(static_cast<size_t>(fsize - total_read), read_buffer_size);
                bytes_read = fread(ptr + total_read, 1, to_read, in_f);
                if (bytes_read == 0) break;
                if (state) XXH64_update(state, ptr + total_read, bytes_read);
                total_read += bytes_read;
            }
            if (total_read == fsize) read_ok = true;
            fclose(in_f);
        } else {
            UI::print_error("Impossibile aprire: " + disk_path);
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

        ::FileEntry fe;
        fe.name = normalize_path(disk_path);
        fe.meta.orig_size = fsize;
        fe.meta.xxhash = h64;
        fe.meta.codec = static_cast<uint8_t>(CodecSelector::select(disk_path, static_cast<size_t>(fsize)));
        
        auto ftime = fs::last_write_time(disk_path, ec);
        if (!ec) {
            fe.meta.timestamp = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                ftime.time_since_epoch()).count());
        } else {
            fe.meta.timestamp = 0;
        }

        // SKIP DEDUP PER FILE .mdb
        bool skip_dedup = should_skip_dedup(disk_path);
        
        if (!skip_dedup && hash_map.count(h64)) {
            fe.meta.is_duplicate = 1;
            fe.meta.duplicate_of_idx = hash_map[h64];
        } else {
            if (!skip_dedup) {
                hash_map[h64] = static_cast<uint32_t>(final_toc.size());
            }
            fe.meta.is_duplicate = 0;
            
            if (solid_buf.size() + fsize > CHUNK_THRESHOLD && !solid_buf.empty()) {
                if (worker_active && !write_worker(future_chunk)) {
                    return {false, "Errore compressione chunk."};
                }
                future_chunk = std::async(std::launch::async, compress_worker, std::move(solid_buf), level, static_cast<Codec>(fe.meta.codec));
                worker_active = true;
                solid_buf.clear();
                
                try {
                    solid_buf.reserve(CHUNK_THRESHOLD);
                } catch (const std::bad_alloc&) {
                    UI::print_warning("Memoria limitata, continuo con chunk più piccoli");
                }
            }
            
            if (solid_buf.capacity() < solid_buf.size() + fsize) {
                size_t new_cap = std::max(solid_buf.capacity() * 2, solid_buf.size() + fsize);
                new_cap = std::min(new_cap, CHUNK_THRESHOLD * 2);
                try {
                    solid_buf.reserve(new_cap);
                } catch (const std::bad_alloc&) {
                    UI::print_error("Memoria esaurita per chunk, forzo scrittura...");
                    if (worker_active && !write_worker(future_chunk)) {
                        return {false, "Memoria insufficiente"};
                    }
                    worker_active = false;
                    solid_buf.clear();
                    try {
                        solid_buf.reserve(CHUNK_THRESHOLD);
                    } catch (const std::bad_alloc&) {
                        return {false, "Memoria insufficiente per continuare"};
                    }
                }
            }
            
            solid_buf.insert(solid_buf.end(), data.begin(), data.end());
            result.bytes_in += fsize;
        }
        
        if (final_toc.size() > MAX_FILES_IN_ARCHIVE) {
            return {false, "Troppi file nell'archivio. Massimo: " + std::to_string(MAX_FILES_IN_ARCHIVE)};
        }
        
        final_toc.push_back(std::move(fe));
    }

    if (worker_active && !write_worker(future_chunk)) {
        return {false, "Errore chunk finale."};
    }
    
    if (!solid_buf.empty()) {
        UI::print_info("Compressione chunk finale...");
        ChunkResult last = compress_worker(std::move(solid_buf), level, Codec::LZMA);
        ::ChunkHeader ch = { 
            static_cast<uint32_t>(last.codec), 
            last.raw_size, 
            static_cast<uint32_t>(last.compressed_data.size()),
            last.data_hash,
            0,  // crc32
            0   // reserved
        };
        if (fwrite(&ch, sizeof(ch), 1, f) != 1) {
            return {false, "Errore scrittura chunk finale."};
        }
        if (fwrite(last.compressed_data.data(), 1, last.compressed_data.size(), f) != last.compressed_data.size()) {
            return {false, "Errore scrittura dati chunk finale."};
        }
        result.bytes_out += last.compressed_data.size();
    }

    ::ChunkHeader end_mark = {0, 0, 0, 0, 0, 0};
    if (fwrite(&end_mark, sizeof(end_mark), 1, f) != 1) {
        return {false, "Errore scrittura marcatore fine."};
    }
    
    if (!IO::write_toc(f, h, final_toc)) {
        return {false, "Errore scrittura TOC."};
    }
    
    // COMMIT: tutto ok, il rollback non deve annullare
    rollback.commit();
    
    result.ok = true;
    UI::print_info("Compressione completata: " + UI::human_size(result.bytes_in) + 
                   " → " + UI::human_size(result.bytes_out) + 
                   " (" + UI::compress_ratio(result.bytes_in, result.bytes_out) + ")");
    
    return result;
}

bool match_pattern(const std::string& full_path, const std::string& pattern) {
    if (pattern.empty()) return true;
    
    std::string target = full_path;
    if (pattern.find('/') == std::string::npos && pattern.find('\\') == std::string::npos) {
        target = fs::path(full_path).filename().string();
    }

    size_t star_pos = pattern.find('*');
    
    if (star_pos == std::string::npos) {
        return target.find(pattern) != std::string::npos;
    }

    std::string prefix = pattern.substr(0, star_pos);
    std::string suffix = pattern.substr(star_pos + 1);

    if (!prefix.empty() && target.find(prefix) != 0) return false;
    if (!suffix.empty()) {
        if (suffix.length() > target.length()) return false;
        if (target.compare(target.length() - suffix.length(), suffix.length(), suffix) != 0) return false;
    }

    return true;
}

TarcResult extract(const std::string& arch_path, const std::vector<std::string>& patterns, bool test_only, size_t offset, bool flat_mode) {
    TarcResult result;
    FILE* f = fopen(arch_path.c_str(), "rb");
    if (!f) {
        return {false, "Archivio non trovato: " + arch_path};
    }

    if (offset > 0) {
        if (fseek(f, static_cast<long>(offset), SEEK_SET) != 0) {
            fclose(f);
            return {false, "Offset non valido."};
        }
    }

    ::Header h{}; 
    if (fread(&h, sizeof(h), 1, f) != 1) {
        fclose(f);
        return {false, "Header corrotto o illeggibile."};
    }
    
    if (std::memcmp(h.magic, TARC_MAGIC, 4) != 0) {
        fclose(f);
        return {false, "Magic number non valido - file non è un archivio TARC."};
    }
    
    std::vector<::FileEntry> toc;
    h.toc_offset += offset;
    
    if (!IO::read_toc(f, h, toc)) {
        fclose(f);
        return {false, "Impossibile leggere TOC."};
    }
    
    if (fseek(f, static_cast<long>(offset + sizeof(::Header)), SEEK_SET) != 0) {
        fclose(f);
        return {false, "Seek fallito dopo header."};
    }
    
    std::vector<char> current_block;
    size_t block_pos = 0;
    std::map<std::string, int> flat_names_counter;
    
    size_t total_to_extract = 0;
    for (const auto& fe : toc) {
        if (patterns.empty()) {
            total_to_extract++;
        } else {
            for (const auto& pat : patterns) {
                if (match_pattern(fe.name, pat)) {
                    total_to_extract++;
                    break;
                }
            }
        }
    }
    
    size_t extracted_count = 0;

    for (size_t i = 0; i < toc.size(); ++i) {
        auto& fe = toc[i];
        
        bool should_extract = patterns.empty();
        if (!should_extract) {
            for (const auto& pat : patterns) {
                if (match_pattern(fe.name, pat)) {
                    should_extract = true;
                    break;
                }
            }
        }

        if (!should_extract) {
            if (!fe.meta.is_duplicate) {
                if (block_pos >= current_block.size()) {
                    ::ChunkHeader ch;
                    if (fread(&ch, sizeof(ch), 1, f) != 1 || ch.raw_size == 0) break;
                    
                    std::vector<char> comp(ch.comp_size);
                    if (fread(comp.data(), 1, ch.comp_size, f) != ch.comp_size) {
                        fclose(f);
                        return {false, "Errore lettura chunk."};
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
                            return {false, "Errore decompressione LZMA (codice: " + std::to_string(ret) + ")"};
                        }
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
            if (fe.meta.duplicate_of_idx < toc.size()) {
                UI::print_extract(fe.name, fe.meta.orig_size, test_only, true);
                result.bytes_out += fe.meta.orig_size;
                extracted_count++;
            } else {
                UI::print_error("Riferimento duplicato non valido: " + fe.name);
            }
            continue;
        }
        
        if (block_pos >= current_block.size()) {
            ::ChunkHeader ch;
            if (fread(&ch, sizeof(ch), 1, f) != 1 || ch.raw_size == 0) {
                UI::print_error("Fine archivio inaspettata");
                break;
            }
            
            std::vector<char> comp(ch.comp_size);
            if (fread(comp.data(), 1, ch.comp_size, f) != ch.comp_size) {
                fclose(f);
                return {false, "Errore lettura dati compressi."};
            }
            
            // Verifica integrità chunk usando checksum
            if (ch.checksum != 0) {
                uint64_t computed_hash = XXH64(comp.data(), ch.comp_size, 0);
                if (computed_hash != ch.checksum) {
                    UI::print_warning("Hash mismatch nel chunk - possibile corruzione dati");
                }
            }
            
            current_block.resize(ch.raw_size);
            
            if (ch.codec == static_cast<uint32_t>(Codec::LZMA)) {
                size_t src_p = 0, dst_p = 0;
                uint64_t limit = UINT64_MAX;
                lzma_ret ret = lzma_stream_buffer_decode(&limit, 0, nullptr,
                    reinterpret_cast<const uint8_t*>(comp.data()), &src_p, ch.comp_size,
                    reinterpret_cast<uint8_t*>(current_block.data()), &dst_p, ch.raw_size);
                    
                const char* err_msg = "Unknown error";
                if (ret != LZMA_OK && ret != LZMA_STREAM_END) {
                    switch(ret) {
                        case LZMA_MEM_ERROR: err_msg = "Memoria insufficiente"; break;
                        case LZMA_FORMAT_ERROR: err_msg = "Formato non valido"; break;
                        case LZMA_DATA_ERROR: err_msg = "Dati corrotti"; break;
                        case LZMA_BUF_ERROR: err_msg = "Buffer insufficiente"; break;
                        default: break;
                    }
                    fclose(f);
                    return {false, "Errore decompressione LZMA: " + std::string(err_msg)};
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
            if (!IO::write_file_to_disk(final_path, current_block.data() + block_pos, fe.meta.orig_size, fe.meta.timestamp)) {
                UI::print_error("Impossibile scrivere: " + final_path);
            }
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
    TarcResult res;
    FILE* f = fopen(arch_path.c_str(), "rb");
    if (!f) {
        return {false, "Archivio non trovato: " + arch_path};
    }
    
    if (offset > 0) {
        if (fseek(f, static_cast<long>(offset), SEEK_SET) != 0) {
            fclose(f);
            return {false, "Offset non valido."};
        }
    }
    
    ::Header h{}; 
    if (fread(&h, sizeof(h), 1, f) != 1) {
        fclose(f);
        return {false, "Errore Header."};
    }
    
    if (std::memcmp(h.magic, TARC_MAGIC, 4) != 0) {
        fclose(f);
        return {false, "Magic number non valido - file non è un archivio TARC."};
    }
    
    std::vector<::FileEntry> toc;
    h.toc_offset += offset;
    
    if (!IO::read_toc(f, h, toc)) {
        fclose(f);
        return {false, "Errore TOC."};
    }
    
    printf("\n%s=== Archivio: %s ===%s\n", Color::CYAN, fs::path(arch_path).filename().string().c_str(), Color::RESET);
    printf("%sFormato: TARC v%d | File: %zu%s\n\n", Color::DIM, h.version, toc.size(), Color::RESET);
    
    uint64_t total_size = 0;
    for (const auto& fe : toc) {
        UI::print_list_entry(fe.name, fe.meta.orig_size, fe.meta.is_duplicate ? 0 : 1, static_cast<Codec>(fe.meta.codec));
        total_size += fe.meta.orig_size;
    }
    
    printf("\n%sTotale: %s%s\n", Color::BOLD, UI::human_size(total_size).c_str(), Color::RESET);
    
    fclose(f);
    res.ok = true; 
    return res;
}

TarcResult remove_files(const std::string&, const std::vector<std::string>&) { 
    return {false, "Rimozione non supportata in modalita' Solid senza riscrittura completa."}; 
}

} // namespace Engine
