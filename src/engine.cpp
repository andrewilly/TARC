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
#include <thread>
#include <queue>
#include <condition_variable>

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
        static const std::set<std::string> skip = { ".zip", ".7z", ".rar", ".gz", ".mp4", ".jpg", ".png" };
        std::string e = ext;
        std::transform(e.begin(), e.end(), e.begin(), ::tolower);
        return skip.find(e) == skip.end();
    }
    
    Codec select(const std::string& path, size_t size) {
        if (!is_compressibile(fs::path(path).extension().string())) return Codec::STORE;
        
        // Selezione codec basata su dimensione e tipo
        if (size < 1024 * 512) return Codec::ZSTD;      // File piccoli: ZSTD veloce
        if (size > 100 * 1024 * 1024) return Codec::LZMA; // File grandi: LZMA
        return Codec::LZMA;  // Default LZMA
    }
}

// Thread pool per compressione asincrona
class ThreadPool {
private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    bool stop;
    
public:
    ThreadPool(size_t threads = std::thread::hardware_concurrency()) : stop(false) {
        for (size_t i = 0; i < threads; ++i) {
            workers.emplace_back([this] {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(this->queue_mutex);
                        this->condition.wait(lock, [this] { return this->stop || !this->tasks.empty(); });
                        if (this->stop && this->tasks.empty()) return;
                        task = std::move(this->tasks.front());
                        this->tasks.pop();
                    }
                    task();
                }
            });
        }
    }
    
    template<class F, class... Args>
    auto enqueue(F&& f, Args&&... args) -> std::future<typename std::result_of<F(Args...)>::type> {
        using return_type = typename std::result_of<F(Args...)>::type;
        
        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );
        
        std::future<return_type> res = task->get_future();
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            if (stop) throw std::runtime_error("enqueue on stopped ThreadPool");
            tasks.emplace([task]() { (*task)(); });
        }
        condition.notify_one();
        return res;
    }
    
    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            stop = true;
        }
        condition.notify_all();
        for (std::thread &worker : workers) worker.join();
    }
};

static std::unique_ptr<ThreadPool> g_pool;
static std::atomic<size_t> g_global_progress{0};
static size_t g_chunk_threshold = 256 * 1024 * 1024; // 256MB

namespace Engine {

void set_skip_dedup_extensions(const std::vector<std::string>& exts) {
    CodecSelector::skip_dedup_exts.clear();
    for (const auto& e : exts) CodecSelector::skip_dedup_exts.insert(e);
}

void set_chunk_threshold(size_t threshold) {
    g_chunk_threshold = threshold;
    UI::print_info("Chunk threshold impostato a " + UI::human_size(threshold));
}

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
    uint64_t content_hash;
};

// Versione migliorata di compress_worker con dizionario adattivo
ChunkResult compress_worker(std::vector<char> raw_data, int level, Codec chosen_codec) {
    ChunkResult res;
    res.raw_size = (uint32_t)raw_data.size();
    res.codec = chosen_codec;
    res.success = false;
    
    if (chosen_codec == Codec::STORE) {
        res.compressed_data = std::move(raw_data);
        res.success = true;
        // Calcola hash per integrità
        res.content_hash = XXH3_64bits(res.compressed_data.data(), res.compressed_data.size());
    } else if (chosen_codec == Codec::LZMA) {
        // Dizionario adattivo basato sulla dimensione
        uint32_t dict_size = 1 << 23; // 8MB default
        
        if (raw_data.size() > 64 * 1024 * 1024) dict_size = 1 << 27;      // 128MB
        else if (raw_data.size() > 16 * 1024 * 1024) dict_size = 1 << 26; // 64MB
        else if (raw_data.size() > 4 * 1024 * 1024) dict_size = 1 << 25;  // 32MB
        else if (raw_data.size() > 1024 * 1024) dict_size = 1 << 24;       // 16MB
        
        lzma_options_lzma opt = LZMA_OPTIONS_INITIALIZER;
        opt.dict_size = dict_size;
        
        // Imposta preset
        uint32_t preset = (level < 0) ? 9 : (uint32_t)level;
        if (lzma_lzma_preset(&opt, preset | LZMA_PRESET_EXTREME) != LZMA_OK) {
            // Fallback a preset standard
            opt.preset = preset;
        }
        
        lzma_stream strm = LZMA_STREAM_INIT;
        lzma_ret ret = lzma_alone_encoder(&strm, &opt);
        
        if (ret == LZMA_OK) {
            size_t max_out = raw_data.size() + (raw_data.size() / 10) + 128;
            res.compressed_data.resize(max_out);
            
            strm.next_in = (const uint8_t*)raw_data.data();
            strm.avail_in = raw_data.size();
            strm.next_out = (uint8_t*)res.compressed_data.data();
            strm.avail_out = max_out;
            
            ret = lzma_code(&strm, LZMA_FINISH);
            
            if (ret == LZMA_STREAM_END) {
                res.compressed_data.resize(strm.total_out);
                res.success = true;
                res.content_hash = XXH3_64bits(res.compressed_data.data(), res.compressed_data.size());
            } else {
                UI::print_warning("LZMA compression failed with code: " + std::to_string(ret));
                // Fallback a STORE
                res.compressed_data = std::move(raw_data);
                res.codec = Codec::STORE;
                res.success = true;
                res.content_hash = XXH3_64bits(res.compressed_data.data(), res.compressed_data.size());
            }
            
            lzma_end(&strm);
        } else {
            UI::print_warning("LZMA encoder initialization failed, falling back to STORE");
            res.compressed_data = std::move(raw_data);
            res.codec = Codec::STORE;
            res.success = true;
            res.content_hash = XXH3_64bits(res.compressed_data.data(), res.compressed_data.size());
        }
    } else {
        // Altri codec (ZSTD, LZ4, BR) - placeholder per ora usa STORE
        res.compressed_data = std::move(raw_data);
        res.codec = Codec::STORE;
        res.success = true;
        res.content_hash = XXH3_64bits(res.compressed_data.data(), res.compressed_data.size());
    }
    
    return res;
}

// Versione migliorata con fseek64
TarcResult create_sfx(const std::string& archive_path, const std::string& sfx_name) {
    std::string stub_path = "tarc_sfx_stub.exe";
    if (!fs::exists(stub_path)) {
        // Cerca nella stessa cartella dell'eseguibile
#ifdef _WIN32
        char module_path[MAX_PATH];
        GetModuleFileNameA(NULL, module_path, MAX_PATH);
        fs::path exe_dir = fs::path(module_path).parent_path();
        stub_path = (exe_dir / "tarc_sfx_stub.exe").string();
        if (!fs::exists(stub_path)) 
            return {false, "Stub SFX (tarc_sfx_stub.exe) non trovato."};
#else
        return {false, "Stub SFX (tarc_sfx_stub.exe) non trovato nella cartella."};
#endif
    }

    std::ifstream stub_in(stub_path, std::ios::binary);
    std::ifstream data_in(archive_path, std::ios::binary);
    std::ofstream sfx_out(sfx_name, std::ios::binary);

    if (!stub_in || !data_in || !sfx_out) return {false, "Errore fatale durante la fusione SFX."};

    sfx_out << stub_in.rdbuf();
    sfx_out << data_in.rdbuf();
    
    return {true, "Archivio autoestraente generato."};
}

TarcResult compress(const std::string& archive_path, const std::vector<std::string>& inputs, bool append, int level) {
    TarcResult result;
    std::vector<std::string> expanded_files;
    
    for (const auto& in : inputs) resolve_wildcards(in, expanded_files);
    if (expanded_files.empty()) return {false, "Nessun file trovato."};
    
    UI::print_info("Preparazione compressione di " + std::to_string(expanded_files.size()) + " file...");
    
    // Inizializza thread pool
    if (!g_pool) g_pool = std::make_unique<ThreadPool>();
    
    std::vector<::FileEntry> final_toc;
    ConcurrentHashMap hash_map;
    ::Header h{};
    
    // Gestione append
    if (append && fs::exists(archive_path)) {
        FILE* f_old = fopen(archive_path.c_str(), "rb");
        if (f_old) {
            (void)fread(&h, sizeof(h), 1, f_old);
            IO::read_toc(f_old, h, final_toc);
            fclose(f_old);
            for (size_t k = 0; k < final_toc.size(); ++k) {
                if (!final_toc[k].meta.is_duplicate) {
                    std::string ext = fs::path(final_toc[k].name).extension().string();
                    hash_map.insert(final_toc[k].meta.xxhash, (uint32_t)k, ext);
                }
            }
        }
    } else {
        memcpy(h.magic, TARC_MAGIC, 4);
        h.version = TARC_VERSION;
    }
    
    // Apertura file con gestione errori migliorata
    FILE* f = fopen(archive_path.c_str(), append ? "rb+" : "wb");
    if (!f) return {false, "ERRORE CRITICO: Impossibile scrivere l'archivio: " + std::string(strerror(errno))};
    
    if (append) {
        if (fseek64(f, (off64_t)h.toc_offset, SEEK_SET) != 0) {
            fclose(f);
            return {false, "ERRORE: Seek fallito in modalità append"};
        }
    } else {
        fwrite(&h, sizeof(h), 1, f);
    }
    
    std::vector<char> solid_buf;
    solid_buf.reserve(g_chunk_threshold);
    
    std::future<ChunkResult> future_chunk;
    bool worker_active = false;
    
    auto write_worker = [&](std::future<ChunkResult>& fut) -> bool {
        ChunkResult res = fut.get();
        if (!res.success) return false;
        
        ::ChunkHeader ch = { 
            (uint32_t)res.codec, 
            res.raw_size, 
            (uint32_t)res.compressed_data.size(),
            res.content_hash
        };
        
        fwrite(&ch, sizeof(ch), 1, f);
        fwrite(res.compressed_data.data(), 1, res.compressed_data.size(), f);
        result.bytes_out += res.compressed_data.size();
        return true;
    };
    
    // Progress tracking
    g_global_progress = 0;
    size_t total_files = expanded_files.size();
    
    for (size_t i = 0; i < expanded_files.size(); ++i) {
        std::string disk_path = expanded_files[i];
        UI::print_progress(i + 1, total_files, fs::path(disk_path).filename().string());
        
        if (!fs::exists(disk_path)) {
            UI::print_warning("File non trovato: " + disk_path);
            continue;
        }
        
        uintmax_t fsize = fs::file_size(disk_path);
        
        // Verifica memoria disponibile
        if (fsize > g_chunk_threshold * 2) {
            UI::print_warning("File molto grande (" + UI::human_size(fsize) + 
                            "), potrebbe richiedere molta memoria");
        }
        
        std::vector<char> data;
        bool read_ok = false;
        uint64_t h64 = 0;
        
        // Allocazione sicura con try-catch
        try {
            data.resize(fsize);
        } catch (const std::bad_alloc& e) {
            UI::print_error("Memoria insufficiente per: " + disk_path + " (" + UI::human_size(fsize) + ")");
            continue;
        }
        
        // Lettura file con XXH3 per hash migliore
        XXH3_state_t* const state = XXH3_createState();
        if (state) XXH3_64bits_reset(state);
        
#ifdef _WIN32
        HANDLE hFile = CreateFileA(disk_path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, 
                                    OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            DWORD bytesReadTotal = 0;
            DWORD bytesRead = 0;
            const DWORD BUF_STEP = 1024 * 1024; // 1MB buffer
            char* ptr = data.data();
            
            while (bytesReadTotal < (DWORD)fsize) {
                DWORD toRead = ((DWORD)fsize - bytesReadTotal > BUF_STEP) ? BUF_STEP : ((DWORD)fsize - bytesReadTotal);
                if (ReadFile(hFile, ptr + bytesReadTotal, toRead, &bytesRead, NULL)) {
                    if (state) XXH3_64bits_update(state, ptr + bytesReadTotal, bytesRead);
                    bytesReadTotal += bytesRead;
                } else {
                    UI::print_error("Errore lettura: " + disk_path + " (WinErr: " + std::to_string(GetLastError()) + ")");
                    break;
                }
            }
            if (bytesReadTotal == (DWORD)fsize) read_ok = true;
            CloseHandle(hFile);
        } else {
            UI::print_error("Accesso negato: " + disk_path + " (Err: " + std::to_string(GetLastError()) + ")");
        }
#else
        FILE* in_f = fopen(disk_path.c_str(), "rb");
        if (in_f) {
            size_t bytes_read = 0;
            size_t total_read = 0;
            char* ptr = data.data();
            
            while (total_read < fsize) {
                bytes_read = fread(ptr + total_read, 1, fsize - total_read, in_f);
                if (bytes_read == 0) break;
                if (state) XXH3_64bits_update(state, ptr + total_read, bytes_read);
                total_read += bytes_read;
            }
            
            if (total_read == fsize) read_ok = true;
            fclose(in_f);
        } else {
            UI::print_error("Impossibile aprire: " + disk_path);
        }
#endif
        
        if (state) {
            h64 = XXH3_64bits_digest(state);
            XXH3_freeState(state);
        }
        
        if (!read_ok) {
            UI::print_error("Lettura fallita per: " + disk_path);
            continue;
        }
        
        ::FileEntry fe;
        fe.name = normalize_path(disk_path);
        fe.meta.orig_size = fsize;
        fe.meta.xxhash = h64;
        fe.meta.codec = (uint8_t)CodecSelector::select(disk_path, fsize);
        fe.meta.timestamp = (uint64_t)std::chrono::duration_cast<std::chrono::seconds>(
            fs::last_write_time(disk_path).time_since_epoch()).count();
        
        // Controllo skip dedup per estensioni problematiche
        bool skip_dedup = CodecSelector::should_skip_dedup(disk_path);
        std::string ext = fs::path(disk_path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        
        if (!skip_dedup && hash_map.contains(h64, ext)) {
            fe.meta.is_duplicate = 1;
            fe.meta.duplicate_of_idx = hash_map.get(h64);
            UI::print_warning("DUPLICATO rilevato: " + fe.name);
        } else {
            if (!skip_dedup) hash_map.insert(h64, (uint32_t)final_toc.size(), ext);
            fe.meta.is_duplicate = 0;
            
            // Gestione chunk con controllo capacità
            if (solid_buf.size() + fsize > g_chunk_threshold && !solid_buf.empty()) {
                if (worker_active && !write_worker(future_chunk)) {
                    fclose(f);
                    return {false, "Errore compressione chunk."};
                }
                
                // Avvia compressione asincrona
                future_chunk = g_pool->enqueue(compress_worker, std::move(solid_buf), level, (Codec)fe.meta.codec);
                worker_active = true;
                solid_buf.clear();
                solid_buf.reserve(g_chunk_threshold);
            }
            
            // Verifica capacità prima di inserire
            if (solid_buf.capacity() < solid_buf.size() + fsize) {
                size_t new_cap = std::max(solid_buf.capacity() * 2, solid_buf.size() + fsize);
                new_cap = std::min(new_cap, g_chunk_threshold * 2);
                try {
                    solid_buf.reserve(new_cap);
                } catch (const std::bad_alloc& e) {
                    UI::print_error("Memoria esaurita per chunk, salvataggio...");
                    if (worker_active && !write_worker(future_chunk)) {
                        fclose(f);
                        return {false, "Memoria insufficiente"};
                    }
                    worker_active = false;
                    solid_buf.clear();
                    solid_buf.reserve(g_chunk_threshold);
                }
            }
            
            solid_buf.insert(solid_buf.end(), data.begin(), data.end());
            result.bytes_in += fsize;
        }
        final_toc.push_back(fe);
        g_global_progress++;
    }
    
    // Finalizza chunk pendenti
    if (worker_active && !write_worker(future_chunk)) {
        fclose(f);
        return {false, "Errore chunk finale."};
    }
    
    if (!solid_buf.empty()) {
        UI::print_info("Compressione chunk finale...");
        ChunkResult last = compress_worker(std::move(solid_buf), level, Codec::LZMA);
        ::ChunkHeader ch = { 
            (uint32_t)last.codec, 
            last.raw_size, 
            (uint32_t)last.compressed_data.size(),
            last.content_hash
        };
        fwrite(&ch, sizeof(ch), 1, f);
        fwrite(last.compressed_data.data(), 1, last.compressed_data.size(), f);
        result.bytes_out += last.compressed_data.size();
    }
    
    // Terminatore chunk
    ::ChunkHeader end_mark = {0, 0, 0, 0};
    fwrite(&end_mark, sizeof(end_mark), 1, f);
    
    IO::write_toc(f, h, final_toc);
    fclose(f);
    
    result.ok = true;
    UI::print_info("Compressione completata: " + UI::human_size(result.bytes_in) + 
                   " → " + UI::human_size(result.bytes_out) + 
                   " (" + UI::compress_ratio(result.bytes_in, result.bytes_out) + ")");
    
    return result;
}

// match_pattern migliorato con supporto wildcard esteso
bool match_pattern(const std::string& full_path, const std::string& pattern) {
    if (pattern.empty()) return true;
    
    std::string target = full_path;
    
    // Se pattern non contiene slash, usa solo filename
    if (pattern.find('/') == std::string::npos && pattern.find('\\') == std::string::npos) {
        target = fs::path(full_path).filename().string();
    }
    
    // Supporto per wildcard multipli
    size_t star_pos = pattern.find('*');
    if (star_pos == std::string::npos) {
        return target.find(pattern) != std::string::npos;
    }
    
    // Supporto pattern come "*.txt" o "prefix*" o "*suffix"
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
    if (!f) return {false, "Archivio non trovato: " + arch_path};
    
    if (offset > 0) {
        if (fseek64(f, (off64_t)offset, SEEK_SET) != 0) {
            fclose(f);
            return {false, "Offset non valido"};
        }
    }
    
    ::Header h; 
    if (fread(&h, sizeof(h), 1, f) != 1) {
        fclose(f);
        return {false, "Header corrotto o illeggibile"};
    }
    
    // Verifica magic number
    if (memcmp(h.magic, TARC_MAGIC, 4) != 0) {
        fclose(f);
        return {false, "Magic number non valido - file non è un archivio TARC"};
    }
    
    std::vector<::FileEntry> toc;
    h.toc_offset += offset;
    
    if (!IO::read_toc(f, h, toc)) {
        fclose(f);
        return {false, "Impossibile leggere TOC"};
    }
    
    if (fseek64(f, (off64_t)(offset + sizeof(::Header)), SEEK_SET) != 0) {
        fclose(f);
        return {false, "Seek fallito dopo header"};
    }
    
    std::vector<char> current_block;
    size_t block_pos = 0;
    std::map<std::string, int> flat_names_counter;
    size_t extracted_count = 0;
    size_t total_to_extract = 0;
    
    // Conta quanti file da estrarre
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
            // Salta file, ma deve avanzare nei chunk se necessario
            if (!fe.meta.is_duplicate) {
                if (block_pos >= current_block.size()) {
                    ::ChunkHeader ch;
                    if (fread(&ch, sizeof(ch), 1, f) != 1 || ch.raw_size == 0) break;
                    
                    std::vector<char> comp(ch.comp_size);
                    if (fread(comp.data(), 1, ch.comp_size, f) != ch.comp_size) break;
                    
                    current_block.resize(ch.raw_size);
                    
                    if (ch.codec == (uint32_t)Codec::LZMA) {
                        size_t src_p = 0, dst_p = 0;
                        uint64_t limit = UINT64_MAX;
                        lzma_ret ret = lzma_stream_buffer_decode(&limit, 0, NULL,
                            (const uint8_t*)comp.data(), &src_p, ch.comp_size,
                            (uint8_t*)current_block.data(), &dst_p, ch.raw_size);
                            
                        if (ret != LZMA_OK && ret != LZMA_STREAM_END) {
                            fclose(f);
                            return {false, "Errore decompressione LZMA (codice: " + std::to_string(ret) + ")"};
                        }
                    } else {
                        memcpy(current_block.data(), comp.data(), ch.raw_size);
                    }
                    block_pos = 0;
                }
                block_pos += fe.meta.orig_size;
            }
            continue;
        }
        
        if (fe.meta.is_duplicate) {
            // Trova il file originale
            if (fe.meta.duplicate_of_idx < toc.size()) {
                UI::print_extract(fe.name, fe.meta.orig_size, test_only, true);
                result.bytes_out += fe.meta.orig_size;
                extracted_count++;
            } else {
                UI::print_error("Riferimento duplicato non valido: " + fe.name);
            }
            continue;
        }
        
        // Carica chunk se necessario
        if (block_pos >= current_block.size()) {
            ::ChunkHeader ch;
            if (fread(&ch, sizeof(ch), 1, f) != 1 || ch.raw_size == 0) {
                UI::print_error("Fine archivio inaspettata");
                break;
            }
            
            std::vector<char> comp(ch.comp_size);
            if (fread(comp.data(), 1, ch.comp_size, f) != ch.comp_size) {
                fclose(f);
                return {false, "Errore lettura dati compressi"};
            }
            
            // Verifica integrità chunk
            uint64_t computed_hash = XXH3_64bits(comp.data(), ch.comp_size);
            if (computed_hash != ch.content_hash && ch.content_hash != 0) {
                UI::print_warning("Hash mismatch nel chunk - possibile corruzione");
            }
            
            current_block.resize(ch.raw_size);
            
            if (ch.codec == (uint32_t)Codec::LZMA) {
                size_t src_p = 0, dst_p = 0;
                uint64_t limit = UINT64_MAX;
                lzma_ret ret = lzma_stream_buffer_decode(&limit, 0, NULL,
                    (const uint8_t*)comp.data(), &src_p, ch.comp_size,
                    (uint8_t*)current_block.data(), &dst_p, ch.raw_size);
                    
                const char* err_msg = "Unknown error";
                if (ret != LZMA_OK && ret != LZMA_STREAM_END) {
                    switch(ret) {
                        case LZMA_MEM_ERROR: err_msg = "Memoria insufficiente"; break;
                        case LZMA_FORMAT_ERROR: err_msg = "Formato non valido"; break;
                        case LZMA_DATA_ERROR: err_msg = "Dati corrotti"; break;
                        case LZMA_BUF_ERROR: err_msg = "Buffer insufficiente"; break;
                    }
                    fclose(f);
                    return {false, "Errore decompressione LZMA: " + std::string(err_msg)};
                }
            } else {
                memcpy(current_block.data(), comp.data(), ch.raw_size);
            }
            block_pos = 0;
        }
        
        UI::print_progress(extracted_count + 1, total_to_extract, fe.name);
        
        std::string final_path = fe.name;
        if (flat_mode) {
            fs::path p(fe.name);
            std::string filename = p.filename().string();
            
            if (flat_names_counter.count(filename)) {
                flat_names_counter[filename]++;
                size_t dot_pos = filename.find_last_of('.');
                if (dot_pos != std::string::npos) {
                    filename = filename.substr(0, dot_pos) + "_" + std::to_string(flat_names_counter[filename]) + filename.substr(dot_pos);
                } else {
                    filename += "_" + std::to_string(flat_names_counter[filename]);
                }
            } else {
                flat_names_counter[filename] = 0;
            }
            final_path = filename;
        }
        
        if (!test_only) {
            if (!IO::write_file_to_disk(final_path, current_block.data() + block_pos, fe.meta.orig_size, fe.meta.timestamp)) {
                UI::print_error("Impossibile scrivere: " + final_path);
            } else {
                UI::print_extract(final_path, fe.meta.orig_size, test_only, true);
            }
        } else {
            UI::print_extract(final_path, fe.meta.orig_size, test_only, true);
        }
        
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
    if (!f) return {false, "Errore apertura archivio"};
    
    if (offset > 0) {
        if (fseek64(f, (off64_t)offset, SEEK_SET) != 0) {
            fclose(f);
            return {false, "Offset non valido"};
        }
    }
    
    ::Header h; 
    if (fread(&h, sizeof(h), 1, f) != 1) {
        fclose(f);
        return {false, "Errore Header"};
    }
    
    std::vector<::FileEntry> toc;
    h.toc_offset += offset;
    
    if (!IO::read_toc(f, h, toc)) {
        fclose(f);
        return {false, "Errore TOC"};
    }
    
    printf("\n%s=== Archivio: %s ===%s\n", Color::CYAN, fs::path(arch_path).filename().string().c_str(), Color::RESET);
    printf("%sFormato: TARC v%d | File: %zu%s\n\n", Color::DIM, h.version, toc.size(), Color::RESET);
    
    uint64_t total_size = 0;
    for (const auto& fe : toc) {
        UI::print_list_entry(fe.name, fe.meta.orig_size, fe.meta.is_duplicate ? 0 : 1, (Codec)fe.meta.codec);
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
