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
#include <atomic>
#include <thread>

#ifdef _WIN32
    #include <windows.h>
#endif

#include "lzma.h"
extern "C" {
    #include "xxhash.h"
    #include "zstd.h"
    #include "lz4.h"
}

namespace fs = std::filesystem;

namespace {
    ProgressCallback* g_progress_callback = nullptr;
    std::atomic<bool> g_cancelled{false};
    Engine::CompressionStats g_stats;
}

void Engine::set_progress_callback(ProgressCallback* callback) {
    g_progress_callback = callback;
}

Engine::CompressionStats Engine::get_stats() {
    return g_stats;
}

void Engine::reset_stats() {
    g_stats = {};
    g_cancelled = false;
}

namespace CodecSelector {
    static const std::set<std::string> skip = { ".zip", ".7z", ".rar", ".gz", ".bz2", ".xz", ".lz", ".7zip", ".strk" };
    static bool initialized = false;

    void init() {
        if (!initialized) {
            initialized = true;
        }
    }
    
    bool is_compressible(const std::string& ext) {
        std::string e = ext;
        std::transform(e.begin(), e.end(), e.begin(), ::tolower);
        return skip.find(e) == skip.end();
    }
    
    // Ottimizzazione: scegli il codec migliore per ogni tipo di file
    // 7zip usa LZMA2 con contesto per massima compressione
    Codec select(const std::string& path, size_t size) {
        std::string ext = fs::path(path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        
        // File gia' compressi - non comprimere
        if (!is_compressible(ext)) return Codec::STORE;
        
        // File di testo/code - LZMA eccelle
        if (ext == ".txt" || ext == ".cpp" || ext == ".h" || ext == ".hpp" ||
            ext == ".c" || ext == ".py" || ext == ".js" || ext == ".ts" ||
            ext == ".json" || ext == ".xml" || ext == ".html" || ext == ".css" ||
            ext == ".sql" || ext == ".md" || ext == ".yaml" || ext == ".yml" ||
            ext == ".log" || ext == ".csv" || ext == ".ini" || ext == ".cfg") {
            return Codec::LZMA;
        }
        
        // Database Access - ZSTD ottimo per dati ripetitivi
        if (ext == ".mdb" || ext == ".accdb" || ext == ".mde" || ext == ".accde" ||
            ext == ".db" || ext == ".sqlite" || ext == ".sqlite3") {
            return Codec::ZSTD;
        }
        
        // Immagini/media - Brotli per alta compressione
        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".gif" ||
            ext == ".bmp" || ext == ".ico" || ext == ".webp") {
            return Codec::BR;
        }
        
        // Documenti Office
        if (ext == ".docx" || ext == ".xlsx" || ext == ".pptx" || ext == ".odt") {
            return Codec::ZSTD;
        }
        
        // PDF - spesso gia compressi ma LZMA puo' aiutare
        if (ext == ".pdf") {
            return Codec::BR;
        }
        
        // File piccoli - LZ4 per velocita'
        if (size < 64 * 1024) {
            return Codec::LZ4;
        }
        
        // Default: LZMA per massima compressione (come 7zip)
        return Codec::LZMA;
    }
}

static void report_progress(size_t current, size_t total, const std::string& current_file) {
    if (g_progress_callback) {
        g_progress_callback->on_progress(current, total, current_file);
    }
}

static void report_warning(const std::string& msg) {
    if (g_progress_callback) {
        g_progress_callback->on_warning(msg);
    }
}

static bool check_cancelled() {
    if (g_cancelled) return true;
    if (g_progress_callback && g_progress_callback->is_cancelled()) {
        g_cancelled = true;
        return true;
    }
    return false;
}

namespace Engine {

std::string normalize_path(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}

struct ChunkResult {
    std::vector<char> compressed_data;
    uint32_t raw_size;
    Codec codec;
    bool success;
    std::string error_message;
};

// Compressione LZMA ottimizzata - simile a 7zip LZMA2
ChunkResult compress_lzma_optimal(const std::vector<char>& raw_data, int level) {
    ChunkResult res;
    res.raw_size = static_cast<uint32_t>(raw_data.size());
    res.codec = Codec::LZMA;
    res.success = false;
    
    if (raw_data.empty()) {
        res.compressed_data = raw_data;
        res.success = true;
        return res;
    }
    
    // Configurazione LZMA simile a 7zip (livello 9 = ultra)
    lzma_options_lzma options;
    lzma_lzma_preset(&options, static_cast<uint32_t>(std::min(level, 9)));
    
    // Override con parametri ottimali per massima compressione
    options.dict_size = std::min(static_cast<size_t>(128 * 1024 * 1024), raw_data.size() + (raw_data.size() / 8));
    options.lc = 4;  // Literal context bits (max per testi)
    options.lp = 0;  // Literal position bits
    options.pb = 2;  // Position bits
    options.mode = LZMA_MODE_NORMAL;
    options.nice_len = 273;
    options.search_depth = 48;
    options.fast_bytes = 32;
    
    lzma_stream stream = LZMA_STREAM_INIT;
    
    lzma_ret ret = lzma_alone_encode(&stream, &options,
        reinterpret_cast<const uint8_t*>(raw_data.data()), raw_data.size());
    
    if (ret != LZMA_OK) {
        // Fallback: prova con easy_buffer_encode
        stream = LZMA_STREAM_INIT;
        size_t max_out = lzma_stream_buffer_bound(raw_data.size());
        res.compressed_data.resize(max_out);
        size_t out_pos = 0;
        
        ret = lzma_easy_buffer_encode(
            static_cast<uint32_t>(level) | LZMA_PRESET_EXTREME,
            LZMA_CHECK_CRC64,
            nullptr,
            reinterpret_cast<const uint8_t*>(raw_data.data()),
            raw_data.size(),
            reinterpret_cast<uint8_t*>(res.compressed_data.data()),
            &out_pos,
            max_out
        );
        
        if (ret == LZMA_OK) {
            res.compressed_data.resize(out_pos);
            res.success = true;
        }
        return res;
    }
    
    // Encode chunks
    std::vector<uint8_t> output;
    output.reserve(raw_data.size());
    
    while (stream.avail_in > 0 || stream.avail_out > 0) {
        uint8_t out_buf[8192];
        stream.avail_out = sizeof(out_buf);
        stream.next_out = out_buf;
        
        ret = lzma_code(&stream, LZMA_RUN);
        size_t produced = sizeof(out_buf) - stream.avail_out;
        output.insert(output.end(), out_buf, out_buf + produced);
        
        if (ret != LZMA_OK) break;
    }
    
    // Finish
    stream.avail_in = 0;
    do {
        uint8_t out_buf[8192];
        stream.avail_out = sizeof(out_buf);
        stream.next_out = out_buf;
        ret = lzma_code(&stream, LZMA_FINISH);
        size_t produced = sizeof(out_buf) - stream.avail_out;
        if (produced > 0) {
            output.insert(output.end(), out_buf, out_buf + produced);
        }
    } while (ret == LZMA_OK);
    
    lzma_end(&stream);
    
    if (ret == LZMA_STREAM_END || output.size() < raw_data.size()) {
        res.compressed_data.assign(reinterpret_cast<char*>(output.data()), output.size());
        res.success = true;
    } else {
        // Non ha compresso, store
        res.compressed_data = raw_data;
        res.codec = Codec::STORE;
        res.success = true;
    }
    
    return res;
}

// Compressione ZSTD ad alto livello
ChunkResult compress_zstd(const std::vector<char>& raw_data, int level) {
    ChunkResult res;
    res.raw_size = static_cast<uint32_t>(raw_data.size());
    res.codec = Codec::ZSTD;
    res.success = false;
    
    if (raw_data.empty()) {
        res.compressed_data = raw_data;
        res.success = true;
        return res;
    }
    
    // ZSTD_compress: usa livello massimo (19) per compressione estrema
    int zstd_level = std::min(level * 2, 19);  // Scala: 1->2, 3->6, 9->18
    
    size_t max_out = ZSTD_compressBound(raw_data.size());
    res.compressed_data.resize(max_out);
    
    size_t compressed = ZSTD_compress(
        res.compressed_data.data(),
        max_out,
        raw_data.data(),
        raw_data.size(),
        zstd_level
    );
    
    if (ZSTD_isError(compressed)) {
        res.compressed_data = raw_data;
        res.codec = Codec::STORE;
        res.success = true;
        return res;
    }
    
    res.compressed_data.resize(compressed);
    res.success = true;
    return res;
}

// Compressione LZ4 ad alto livello
ChunkResult compress_lz4(const std::vector<char>& raw_data, int level) {
    ChunkResult res;
    res.raw_size = static_cast<uint32_t>(raw_data.size());
    res.codec = Codec::LZ4;
    res.success = false;
    
    if (raw_data.empty()) {
        res.compressed_data = raw_data;
        res.success = true;
        return res;
    }
    
    // LZ4_compress_default: veloce ma buona compressione
    int lz4_level = (level >= 6) ? 2 : 1;  // HC per livelli alti
    
    int max_out = LZ4_compressBound(static_cast<int>(raw_data.size()));
    std::string compressed(LZ4_compressBound(max_out), '\0');
    
    int compressed_size;
    if (level >= 6) {
        compressed_size = LZ4_compress_HC(
            raw_data.data(),
            compressed.data(),
            static_cast<int>(raw_data.size()),
            max_out,
            LZ4_CLEVEL_MAX
        );
    } else {
        compressed_size = LZ4_compress_default(
            raw_data.data(),
            compressed.data(),
            static_cast<int>(raw_data.size()),
            max_out
        );
    }
    
    if (compressed_size > 0 && compressed_size < static_cast<int>(raw_data.size())) {
        res.compressed_data.assign(compressed.data(), compressed_size);
        res.success = true;
    } else {
        res.compressed_data = raw_data;
        res.codec = Codec::STORE;
        res.success = true;
    }
    
    return res;
}

// Compressione Brotli
ChunkResult compress_brotli(const std::vector<char>& raw_data, int level) {
    ChunkResult res;
    res.raw_size = static_cast<uint32_t>(raw_data.size());
    res.codec = Codec::BR;
    res.success = false;
    
    if (raw_data.empty()) {
        res.compressed_data = raw_data;
        res.success = true;
        return res;
    }
    
    // Brotli compression - level 11 = max
    int brotli_level = std::min(level + 2, 11);
    
    size_t max_out = raw_data.size();
    res.compressed_data.resize(max_out);
    
    size_t compressed = BrotliCompressBuffer(
        brotli_level,
        BROTLI_DEFAULT_WINDOW,
        BROTLI_DEFAULT_MODE,
        raw_data.size(),
        reinterpret_cast<const uint8_t*>(raw_data.data()),
        max_out,
        reinterpret_cast<uint8_t*>(res.compressed_data.data())
    );
    
    if (compressed > 0 && compressed < raw_data.size()) {
        res.compressed_data.resize(compressed);
        res.success = true;
    } else {
        res.compressed_data = raw_data;
        res.codec = Codec::STORE;
        res.success = true;
    }
    
    return res;
}

// Worker principale - sceglie il codec migliore
ChunkResult compress_worker(std::vector<char> raw_data, int level, Codec chosen_codec) {
    ChunkResult res;
    res.raw_size = static_cast<uint32_t>(raw_data.size());
    res.codec = chosen_codec;
    res.success = false;
    
    // Skip piccoli file - non vale la pena comprimere
    if (raw_data.size() < 4096) {
        res.compressed_data = std::move(raw_data);
        res.codec = Codec::STORE;
        res.success = true;
        return res;
    }
    
    // Se STORE, non comprimere
    if (chosen_codec == Codec::STORE) {
        res.compressed_data = std::move(raw_data);
        res.success = true;
        return res;
    }
    
    // Prova il codec scelto
    ChunkResult chosen;
    switch (chosen_codec) {
        case Codec::LZMA:
            chosen = compress_lzma_optimal(raw_data, level);
            break;
        case Codec::ZSTD:
            chosen = compress_zstd(raw_data, level);
            break;
        case Codec::LZ4:
            chosen = compress_lz4(raw_data, level);
            break;
        case Codec::BR:
            chosen = compress_brotli(raw_data, level);
            break;
        default:
            chosen.compressed_data = raw_data;
            chosen.codec = Codec::STORE;
            chosen.success = true;
            break;
    }
    
    // Se il codec scelto non ha compresso bene, prova alternatives
    if (chosen.codec != Codec::STORE && chosen.compressed_data.size() >= raw_data.size() * 0.95) {
        // Prova ZSTD come fallback per LZMA
        if (chosen_codec == Codec::LZMA) {
            auto zstd_res = compress_zstd(raw_data, level);
            if (zstd_res.compressed_data.size() < chosen.compressed_data.size()) {
                return zstd_res;
            }
        }
    }
    
    return chosen;
}

bool decompress_chunk(const std::vector<char>& compressed, std::vector<char>& decompressed, Codec codec) {
    if (codec == Codec::STORE) {
        decompressed = compressed;
        return true;
    }
    
    if (codec == Codec::LZMA) {
        decompressed.resize(256 * 1024 * 1024);
        size_t src_pos = 0, dst_pos = 0;
        uint64_t limit = UINT64_MAX;
        
        lzma_ret ret = lzma_stream_buffer_decode(
            &limit, 0, nullptr,
            reinterpret_cast<const uint8_t*>(compressed.data()),
            &src_pos, compressed.size(),
            reinterpret_cast<uint8_t*>(decompressed.data()),
            &dst_pos, decompressed.size()
        );
        
        if (ret == LZMA_OK || ret == LZMA_STREAM_END) {
            decompressed.resize(dst_pos);
            return true;
        }
        return false;
    }
    
    if (codec == Codec::ZSTD) {
        size_t d_size = ZSTD_decompressBound(compressed.data(), compressed.size());
        decompressed.resize(d_size);
        size_t actual = ZSTD_decompress(
            decompressed.data(),
            d_size,
            compressed.data(),
            compressed.size()
        );
        if (!ZSTD_isError(actual)) {
            decompressed.resize(actual);
            return true;
        }
        return false;
    }
    
    if (codec == Codec::LZ4) {
        int decomp_size = LZ4_decompress_safe(
            compressed.data(),
            decompressed.data(),
            static_cast<int>(compressed.size()),
            static_cast<int>(decompressed.size())
        );
        if (decomp_size > 0) {
            decompressed.resize(decomp_size);
            return true;
        }
        return false;
    }
    
    // Fallback per unknown codec - try LZMA decode
    decompressed.resize(256 * 1024 * 1024);
    size_t src_pos = 0, dst_pos = 0;
    uint64_t limit = UINT64_MAX;
    
    lzma_ret ret = lzma_stream_buffer_decode(
        &limit, 0, nullptr,
        reinterpret_cast<const uint8_t*>(compressed.data()),
        &src_pos, compressed.size(),
        reinterpret_cast<uint8_t*>(decompressed.data()),
        &dst_pos, decompressed.size()
    );
    
    if (ret == LZMA_OK || ret == LZMA_STREAM_END) {
        decompressed.resize(dst_pos);
        return true;
    }
    return false;
}

TarcResult create_sfx(const std::string& archive_path, const std::string& sfx_name) {
    TarcResult res;
    res.ok = false;
    
    std::string stub_path = "tarc_sfx_stub.exe";
    if (!fs::exists(stub_path)) {
        res.error = TarcError::FileNotFound;
        res.message = "Stub SFX (tarc_sfx_stub.exe) not found in current directory.";
        return res;
    }

    std::ifstream stub_in(stub_path, std::ios::binary);
    std::ifstream data_in(archive_path, std::ios::binary);
    std::ofstream sfx_out(sfx_name, std::ios::binary);

    if (!stub_in || !data_in || !sfx_out) {
        res.error = TarcError::AccessDenied;
        res.message = "Failed to create SFX archive.";
        return res;
    }

    sfx_out << stub_in.rdbuf();
    sfx_out << data_in.rdbuf();
    
    res.ok = true;
    res.message = "SFX archive created successfully.";
    return res;
}

TarcResult compress(const std::string& arch_path, const std::vector<std::string>& inputs, bool append, int level) {
    TarcResult res;
    res.ok = false;
    reset_stats();
    
    CodecSelector::init();
    
    std::vector<std::string> expanded_files;
    for (const auto& in : inputs) {
        IO::expand_path(in, expanded_files);
    }
    if (expanded_files.empty()) {
        res.error = TarcError::FileNotFound;
        res.message = "No files found.";
        return res;
    }

    std::vector<FileEntry> final_toc;
    std::map<uint64_t, uint32_t> hash_map;
    Header h{};

    if (append && fs::exists(arch_path)) {
        FILE* f_old = fopen(arch_path.c_str(), "rb");
        if (f_old) {
            if (fread(&h, sizeof(h), 1, f_old) == 1) {
                IO::read_toc(f_old, h, final_toc);
                for (size_t k = 0; k < final_toc.size(); ++k) {
                    if (!final_toc[k].meta.is_duplicate) {
                        hash_map[final_toc[k].meta.xxhash] = static_cast<uint32_t>(k);
                    }
                }
            }
            fclose(f_old);
        }
    } else {
        std::memcpy(h.magic, TARC_MAGIC, 4);
        h.version = TARC_VERSION;
    }

    FILE* f = fopen(arch_path.c_str(), append ? "rb+" : "wb");
    if (!f) {
        res.error = TarcError::AccessDenied;
        res.message = "Cannot write archive.";
        return res;
    }

    if (append) {
        fseek(f, static_cast<long>(h.toc_offset), SEEK_SET);
    } else {
        fwrite(&h, sizeof(h), 1, f);
    }

    // Solid block piu' grande per maggiore compressione (512MB)
    constexpr size_t CHUNK_THRESHOLD = 512 * 1024 * 1024;
    std::vector<char> solid_buf;
    solid_buf.reserve(CHUNK_THRESHOLD);
    
    std::future<ChunkResult> future_chunk;
    bool worker_active = false;
    Codec last_codec = Codec::LZMA;

    auto write_worker = [&](std::future<ChunkResult>& fut) -> bool {
        if (check_cancelled()) return false;
        
        ChunkResult cr = fut.get();
        if (!cr.success) return false;
        
        ChunkHeader ch = {
            static_cast<uint32_t>(cr.codec),
            cr.raw_size,
            static_cast<uint32_t>(cr.compressed_data.size()),
            0
        };
        
        if (fwrite(&ch, sizeof(ch), 1, f) != 1) return false;
        if (fwrite(cr.compressed_data.data(), 1, cr.compressed_data.size(), f) != cr.compressed_data.size()) return false;
        
        res.bytes_out += cr.compressed_data.size();
        return true;
    };

    auto start_time = std::chrono::steady_clock::now();
    
    for (size_t i = 0; i < expanded_files.size(); ++i) {
        if (check_cancelled()) {
            res.error = TarcError::Cancelled;
            res.message = "Operation cancelled by user.";
            fclose(f);
            return res;
        }
        
        const std::string& disk_path = expanded_files[i];
        report_progress(i + 1, expanded_files.size(), fs::path(disk_path).filename().string());

        if (!fs::exists(disk_path)) continue;
        
        uintmax_t fsize = fs::file_size(disk_path);
        
        std::vector<char> data;
        try {
            data.resize(static_cast<size_t>(fsize));
        } catch (const std::bad_alloc&) {
            res.error = TarcError::OutOfMemory;
            res.message = "Insufficient memory: " + disk_path;
            report_warning(res.message);
            continue;
        }

        XXH64_state_t* const state = XXH64_createState();
        if (state) XXH64_reset(state, 0);

        bool read_ok = false;
        uint64_t h64 = 0;
        
#ifdef _WIN32
        HANDLE hFile = CreateFileA(
            disk_path.c_str(), 
            GENERIC_READ, 
            FILE_SHARE_READ | FILE_SHARE_WRITE, 
            nullptr, 
            OPEN_EXISTING, 
            FILE_ATTRIBUTE_NORMAL, 
            nullptr
        );
        
        if (hFile != INVALID_HANDLE_VALUE) {
            DWORD bytesReadTotal = 0;
            DWORD bytesRead = 0;
            constexpr DWORD BUF_STEP = 1024 * 1024;
            char* ptr = data.data();
            
            while (bytesReadTotal < static_cast<DWORD>(fsize)) {
                if (check_cancelled()) {
                    CloseHandle(hFile);
                    break;
                }
                
                DWORD toRead = (static_cast<DWORD>(fsize) - bytesReadTotal > BUF_STEP) 
                    ? BUF_STEP 
                    : (static_cast<DWORD>(fsize) - bytesReadTotal);
                    
                if (ReadFile(hFile, ptr + bytesReadTotal, toRead, &bytesRead, nullptr)) {
                    if (state) XXH64_update(state, ptr + bytesReadTotal, bytesRead);
                    bytesReadTotal += bytesRead;
                } else {
                    report_warning("Read error: " + disk_path + " (WinErr: " + std::to_string(GetLastError()) + ")");
                    break;
                }
            }
            
            if (bytesReadTotal == static_cast<DWORD>(fsize)) read_ok = true;
            CloseHandle(hFile);
        } else {
            report_warning("Access denied: " + disk_path);
        }
#else
        FILE* in_f = fopen(disk_path.c_str(), "rb");
        if (in_f) {
            size_t read_res = fread(data.data(), 1, static_cast<size_t>(fsize), in_f);
            if (read_res == static_cast<size_t>(fsize)) {
                read_ok = true;
                if (state) XXH64_update(state, data.data(), static_cast<size_t>(fsize));
            }
            fclose(in_f);
        }
#endif

        if (state) {
            h64 = XXH64_digest(state);
            XXH64_freeState(state);
        }

        if (!read_ok) continue;

        FileEntry fe;
        fe.name = normalize_path(disk_path);
        fe.meta.orig_size = fsize;
        fe.meta.xxhash = h64;
        fe.meta.codec = static_cast<uint8_t>(CodecSelector::select(disk_path, fsize));
        fe.meta.timestamp = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                fs::last_write_time(disk_path).time_since_epoch()
            ).count()
        );

        if (hash_map.count(h64)) {
            fe.meta.is_duplicate = 1;
            fe.meta.duplicate_of_idx = hash_map[h64];
            g_stats.duplicates_skipped++;
        } else {
            hash_map[h64] = static_cast<uint32_t>(final_toc.size());
            fe.meta.is_duplicate = 0;
            
            // Flush solid block quando pieno o codec cambia
            if (solid_buf.size() + fsize > CHUNK_THRESHOLD && !solid_buf.empty()) {
                if (worker_active && !write_worker(future_chunk)) {
                    res.error = TarcError::CompressionFailed;
                    res.message = "Chunk compression failed.";
                    fclose(f);
                    return res;
                }
                future_chunk = std::async(
                    std::launch::async, 
                    compress_worker, 
                    std::move(solid_buf), 
                    level, 
                    last_codec
                );
                worker_active = true;
                solid_buf.clear();
                solid_buf.reserve(CHUNK_THRESHOLD);
            }
            
            solid_buf.insert(solid_buf.end(), data.begin(), data.end());
            last_codec = static_cast<Codec>(fe.meta.codec);
            g_stats.bytes_read += fsize;
        }
        
        final_toc.push_back(fe);
        g_stats.files_processed++;
    }

    if (worker_active && !write_worker(future_chunk)) {
        res.error = TarcError::CompressionFailed;
        res.message = "Final chunk compression failed.";
        fclose(f);
        return res;
    }
    
    if (!solid_buf.empty()) {
        ChunkResult last = compress_worker(std::move(solid_buf), level, last_codec);
        ChunkHeader ch = {
            static_cast<uint32_t>(last.codec),
            last.raw_size,
            static_cast<uint32_t>(last.compressed_data.size()),
            0
        };
        fwrite(&ch, sizeof(ch), 1, f);
        fwrite(last.compressed_data.data(), 1, last.compressed_data.size(), f);
    }

    ChunkHeader end_mark = {0, 0, 0, 0};
    fwrite(&end_mark, sizeof(end_mark), 1, f);
    IO::write_toc(f, h, final_toc);
    fclose(f);
    
    g_stats.bytes_in = g_stats.bytes_read;
    g_stats.bytes_out = res.bytes_out;
    g_stats.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time
    );
    
    res.ok = true;
    res.bytes_in = g_stats.bytes_read;
    res.bytes_out = res.bytes_out;
    res.message = "Compression completed successfully.";
    return res;
}

static bool match_pattern(const std::string& full_path, const std::string& pattern) {
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
    TarcResult res;
    res.ok = false;
    reset_stats();
    
    FILE* f = fopen(arch_path.c_str(), "rb");
    if (!f) {
        res.error = TarcError::FileNotFound;
        res.message = "Archive not found.";
        return res;
    }

    if (offset > 0) {
        fseek(f, static_cast<long>(offset), SEEK_SET);
    }

    Header h; 
    if (fread(&h, sizeof(h), 1, f) != 1) {
        fclose(f);
        res.error = TarcError::InvalidHeader;
        res.message = "Invalid or corrupted header.";
        return res;
    }
    
    std::vector<FileEntry> toc;
    h.toc_offset += offset;
    if (!IO::read_toc(f, h, toc)) {
        fclose(f);
        res.error = TarcError::CorruptedArchive;
        res.message = "Cannot read TOC.";
        return res;
    }
    
    fseek(f, static_cast<long>(offset + sizeof(Header)), SEEK_SET);
    
    std::vector<char> current_block;
    size_t block_pos = 0;
    std::map<std::string, int> flat_names_counter;

    for (size_t i = 0; i < toc.size(); ++i) {
        if (check_cancelled()) {
            res.error = TarcError::Cancelled;
            res.message = "Operation cancelled.";
            fclose(f);
            return res;
        }
        
        auto& fe = toc[i];
        report_progress(i + 1, toc.size(), fe.name);
        
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
            if (fe.meta.is_duplicate) continue;
            
            if (block_pos >= current_block.size()) {
                ChunkHeader ch;
                if (fread(&ch, sizeof(ch), 1, f) != 1 || ch.raw_size == 0) break;
                
                std::vector<char> comp(ch.comp_size);
                if (fread(comp.data(), 1, ch.comp_size, f) != ch.comp_size) {
                    fclose(f);
                    res.error = TarcError::CorruptedArchive;
                    res.message = "Error reading chunk.";
                    return res;
                }
                
                current_block.resize(ch.raw_size);
                
                lzma_ret ret = LZMA_OK;
                Codec codec = static_cast<Codec>(ch.codec);
                
                if (!decompress_chunk(comp, current_block, codec)) {
                    fclose(f);
                    res.error = TarcError::DecompressionFailed;
                    res.message = "Chunk decompression failed.";
                    return res;
                }
                block_pos = 0;
            }
            block_pos += fe.meta.orig_size;
            continue; 
        }

        if (fe.meta.is_duplicate) continue;
        
        if (block_pos >= current_block.size()) {
            ChunkHeader ch;
            if (fread(&ch, sizeof(ch), 1, f) != 1 || ch.raw_size == 0) break;
            
            std::vector<char> comp(ch.comp_size);
            if (fread(comp.data(), 1, ch.comp_size, f) != ch.comp_size) {
                fclose(f);
                res.error = TarcError::CorruptedArchive;
                res.message = "Error reading compressed data.";
                return res;
            }
            
            current_block.resize(ch.raw_size);
            
            Codec codec = static_cast<Codec>(ch.codec);
            if (!decompress_chunk(comp, current_block, codec)) {
                fclose(f);
                res.error = TarcError::DecompressionFailed;
                res.message = "Decompression failed.";
                return res;
            }
            block_pos = 0;
        }
        
        std::string final_path = fe.name;
        if (flat_mode) {
            fs::path p(fe.name);
            std::string filename = p.filename().string();
            
            if (flat_names_counter.count(filename)) {
                flat_names_counter[filename]++;
                size_t dot_pos = filename.find_last_of('.');
                if (dot_pos != std::string::npos) {
                    filename = filename.substr(0, dot_pos) + "_" + 
                              std::to_string(flat_names_counter[filename]) + 
                              filename.substr(dot_pos);
                } else {
                    filename += "_" + std::to_string(flat_names_counter[filename]);
                }
            } else {
                flat_names_counter[filename] = 0;
            }
            final_path = filename;
        }

        if (!test_only) {
            if (!IO::write_file_to_disk(final_path, current_block.data() + block_pos, 
                                   static_cast<size_t>(fe.meta.orig_size), 
                                   fe.meta.timestamp)) {
                res.error = TarcError::AccessDenied;
                res.message = "Failed to write: " + final_path;
                fclose(f);
                return res;
            }
        }
        
        res.bytes_out += fe.meta.orig_size;
        block_pos += fe.meta.orig_size;
        g_stats.files_processed++;
    }
    
    fclose(f);
    res.ok = true;
    res.message = test_only ? "Test completed successfully." : "Extraction completed successfully.";
    return res;
}

TarcResult list(const std::string& arch_path, size_t offset) {
    TarcResult res;
    res.ok = false;
    
    FILE* f = fopen(arch_path.c_str(), "rb");
    if (!f) {
        res.error = TarcError::FileNotFound;
        res.message = "Archive not found.";
        return res;
    }
    
    if (offset > 0) {
        fseek(f, static_cast<long>(offset), SEEK_SET);
    }
    
    Header h; 
    if (fread(&h, sizeof(h), 1, f) != 1) {
        fclose(f);
        res.error = TarcError::InvalidHeader;
        res.message = "Invalid header.";
        return res;
    }
    
    std::vector<FileEntry> toc;
    h.toc_offset += offset;
    if (!IO::read_toc(f, h, toc)) {
        fclose(f);
        res.error = TarcError::CorruptedArchive;
        res.message = "Cannot read TOC.";
        return res;
    }
    
    for (const auto& fe : toc) {
        UI::print_list_entry(
            fe.name, 
            fe.meta.orig_size, 
            fe.meta.is_duplicate ? 0 : 1, 
            static_cast<Codec>(fe.meta.codec)
        );
    }
    
    fclose(f);
    res.ok = true;
    res.message = "Listed " + std::to_string(toc.size()) + " files.";
    return res;
}

TarcResult remove_files(const std::string&, const std::vector<std::string>&) { 
    TarcResult res;
    res.ok = false;
    res.error = TarcError::Unknown;
    res.message = "Remove operation not supported in Solid mode without full rewrite.";
    return res;
}

}