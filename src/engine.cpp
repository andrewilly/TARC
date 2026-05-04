#include "engine.h"
#include "io.h"
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
}

namespace fs = std::filesystem;

// ARCH-003: safe_now() removed from here — now centralized in types.h
// ARCH-005: AsyncWriter and WriteRequest removed (dead code)

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

// ARCH-013: CodecSelector moved inside Engine namespace (was at file scope)
namespace CodecSelector {
    static const std::set<std::string> skip = { ".zip", ".7z", ".rar", ".gz", ".bz2", ".xz", ".lz", ".7zip", ".strk" };

    bool is_compressible(const std::string& ext) {
        std::string e = ext;
        std::transform(e.begin(), e.end(), e.begin(), ::tolower);
        return skip.find(e) == skip.end();
    }

    Codec select(const std::string& path, size_t size) {
        std::string ext = fs::path(path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        if (!is_compressible(ext)) return Codec::STORE;

        if (ext == ".txt" || ext == ".cpp" || ext == ".h" || ext == ".hpp" ||
            ext == ".c" || ext == ".py" || ext == ".js" || ext == ".ts" ||
            ext == ".json" || ext == ".xml" || ext == ".html" || ext == ".css" ||
            ext == ".sql" || ext == ".md" || ext == ".yaml" || ext == ".yml" ||
            ext == ".log" || ext == ".csv" || ext == ".ini" || ext == ".cfg") {
            return Codec::LZMA;
        }

        if (ext == ".mdb" || ext == ".accdb" || ext == ".mde" || ext == ".accde" ||
            ext == ".db" || ext == ".sqlite" || ext == ".sqlite3") {
            return Codec::ZSTD;
        }

        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".gif" ||
            ext == ".bmp" || ext == ".ico" || ext == ".webp") {
            return Codec::LZMA;
        }

        if (ext == ".docx" || ext == ".xlsx" || ext == ".pptx" || ext == ".odt") {
            return Codec::LZMA;
        }

        if (size < 64 * 1024) {
            return Codec::LZ4;
        }

        return Codec::LZMA;
    }
} // namespace CodecSelector

std::string normalize_path(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    // BUG FIX #7: rimuovi prefisso ./ iniziale
    while (path.size() >= 2 && path[0] == '.' && path[1] == '/') {
        path = path.substr(2);
    }
    return path;
}

struct ChunkResult {
    std::vector<char> compressed_data;
    uint32_t raw_size;
    Codec codec;
    bool success;
    std::string error_message;
};

// ============================================================
// Helper: write a chunk (ChunkHeader + data) to file
// Defined BEFORE create_sfx and compress to avoid "not declared" errors
// ============================================================
// ARCH-008: Compute XXH64 checksum of compressed data
static bool write_chunk(FILE* f, Codec codec, uint32_t raw_size,
                        const std::vector<char>& data, uint64_t& bytes_out) {
    ChunkHeader ch = {
        static_cast<uint32_t>(codec),
        raw_size,
        static_cast<uint32_t>(data.size()),
        0
    };

    // ARCH-008: Compute XXH64 checksum of compressed data
    if (!data.empty()) {
        ch.checksum = XXH64(data.data(), data.size(), 0);
    }

    if (fwrite(&ch, sizeof(ch), 1, f) != 1) return false;
    if (!data.empty()) {
        if (fwrite(data.data(), 1, data.size(), f) != data.size()) return false;
    }
    bytes_out += sizeof(ch) + data.size();  // ARCH-008 FIX: include ChunkHeader size in bytes_out
    return true;
}

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

    size_t max_out = lzma_stream_buffer_bound(raw_data.size());
    res.compressed_data.resize(max_out);
    size_t out_pos = 0;

    uint32_t preset = static_cast<uint32_t>(std::min(level, 9));
    if (level >= 7) {
        preset |= LZMA_PRESET_EXTREME;
    }

    lzma_ret ret = lzma_easy_buffer_encode(
        preset,
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
    } else {
        res.compressed_data = raw_data;
        res.codec = Codec::STORE;
        res.success = true;
    }
    return res;
}

bool decompress_lzma(const std::vector<char>& compressed, std::vector<char>& decompressed) {
    if (decompressed.empty()) {
        decompressed.resize(1024 * 1024);
    }
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

ChunkResult compress_worker(std::vector<char> raw_data, int level, Codec chosen_codec) {
    ChunkResult res;
    res.raw_size = static_cast<uint32_t>(raw_data.size());
    res.codec = chosen_codec;
    res.success = false;

    if (raw_data.empty()) {
        res.compressed_data = std::move(raw_data);
        res.success = true;
        return res;
    }

    if (raw_data.size() < 4096) {
        res.compressed_data = std::move(raw_data);
        res.codec = Codec::STORE;
        res.success = true;
        return res;
    }

    if (chosen_codec == Codec::STORE) {
        res.compressed_data = std::move(raw_data);
        res.success = true;
        return res;
    }

    // Per ora solo LZMA e' implementato; gli altri codec sono placeholder
    // TODO: implementare LZ4, ZSTD, Brotli quando disponibili
    uint32_t saved_raw_size = res.raw_size;
    res = compress_lzma_optimal(raw_data, level);
    res.raw_size = saved_raw_size;

    res.codec = Codec::LZMA;
    return res;
}

// FIX 5 (ARCH-014): decompress_chunk dispatch — proper codec routing with fallback
bool decompress_chunk(const std::vector<char>& compressed, std::vector<char>& decompressed, Codec codec) {
    if (codec == Codec::STORE) {
        decompressed = compressed;
        return true;
    }

    // Currently only LZMA is implemented; other codecs fall back to LZMA
    // TODO: implement native ZSTD, LZ4, Brotli decompression
    if (codec == Codec::LZMA) {
        return decompress_lzma(compressed, decompressed);
    }

    // For unimplemented codecs (ZSTD, LZ4, BR), try LZMA as fallback
    // This handles archives created before codec diversification
    report_warning("Codec " + std::string(codec_name(codec)) + " not natively supported, falling back to LZMA");
    return decompress_lzma(compressed, decompressed);
}

TarcResult create_sfx(const std::string& archive_path, const std::string& sfx_name) {
    TarcResult res;
    res.ok = false;

    std::string stub_path = "tarc_sfx_stub.exe";
    if (!fs::exists(stub_path)) {
        res.error = TarcError::FileNotFound;
        res.message = "Stub SFX not found.";
        return res;
    }

    std::ifstream stub_in(stub_path, std::ios::binary);
    std::ifstream data_in(archive_path, std::ios::binary);
    std::ofstream sfx_out(sfx_name, std::ios::binary);

    if (!stub_in || !data_in || !sfx_out) {
        res.error = TarcError::AccessDenied;
        res.message = "Failed to create SFX.";
        return res;
    }

    sfx_out << stub_in.rdbuf();
    sfx_out << data_in.rdbuf();

    res.ok = true;
    res.message = "SFX created.";
    return res;
}

// ARCH-006: Use CompressOptions struct in API
TarcResult compress(const std::string& arch_path, const std::vector<std::string>& inputs, const CompressOptions& opts) {
    int level = opts.level;
    TarcResult res;
    res.ok = false;
    reset_stats();

    // FIX 4b: TODO — verify option not yet implemented
    // TODO: verify option — re-read and hash-check after write
    (void)opts.verify;

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

    std::memcpy(h.magic, TARC_MAGIC, 4);
    h.version = TARC_VERSION;

    // ARCH-010: RAII wrapper for FILE*
    FileGuard fg(fopen(arch_path.c_str(), "wb"));
    if (!fg) {
        res.error = TarcError::AccessDenied;
        res.message = "Cannot write archive.";
        return res;
    }

    if (fwrite(&h, sizeof(h), 1, fg.get()) != 1) {
        res.error = TarcError::WriteFailed;
        res.message = "Failed to write header.";
        return res;
    }

    // FIX 3: Use opts.chunk_size instead of hardcoded 1GB threshold
    const size_t chunk_threshold = opts.chunk_size;
    std::vector<char> solid_buf;
    solid_buf.reserve(chunk_threshold);

    std::future<ChunkResult> future_chunk;
    bool worker_active = false;

    auto write_worker = [&](std::future<ChunkResult>& fut) -> bool {
        if (check_cancelled()) return false;

        ChunkResult cr = fut.get();
        if (!cr.success) return false;

        if (!write_chunk(fg.get(), cr.codec, cr.raw_size, cr.compressed_data, res.bytes_out)) {
            return false;
        }

        return true;
    };

    auto start_time = safe_now();

    for (size_t i = 0; i < expanded_files.size(); ++i) {
        if (check_cancelled()) {
            res.error = TarcError::Cancelled;
            res.message = "Cancelled.";
            return res;
        }

        const std::string& disk_path = expanded_files[i];
        report_progress(i + 1, expanded_files.size(), fs::path(disk_path).filename().string());

        if (!fs::exists(disk_path)) {
            report_warning("File not found: " + disk_path);
            continue;
        }

        uintmax_t fsize = fs::file_size(disk_path);

        // Controllo overflow: su piattaforme a 32-bit
        if (fsize > static_cast<uintmax_t>(SIZE_MAX)) {
            report_warning("File too large (overflow): " + disk_path);
            continue;
        }

        std::vector<char> data;
        try {
            data.resize(static_cast<size_t>(fsize));
        } catch (const std::bad_alloc&) {
            res.error = TarcError::OutOfMemory;
            res.message = "Out of memory: " + disk_path;
            report_warning(res.message);
            continue;
        }

        bool read_ok = false;
        uint64_t h64 = 0;
        XXH64_state_t* const state = XXH64_createState();
        if (state) XXH64_reset(state, 0);

        FILE* in_f = fopen(disk_path.c_str(), "rb");
        if (in_f) {
            size_t read_res = fread(data.data(), 1, static_cast<size_t>(fsize), in_f);
            if (read_res == static_cast<size_t>(fsize)) {
                read_ok = true;
                if (state) XXH64_update(state, data.data(), static_cast<size_t>(fsize));
            }
            fclose(in_f);
        } else {
            report_warning("Cannot open file: " + disk_path);
        }

        if (state) {
            h64 = XXH64_digest(state);
            XXH64_freeState(state);
        }

        if (!read_ok) {
            report_warning("Failed to read file: " + disk_path);
            continue;
        }

        FileEntry fe;
        fe.name = normalize_path(disk_path);
        fe.meta.orig_size = fsize;
        fe.meta.xxhash = h64;

        constexpr size_t STORE_THRESHOLD = 2048;

        // BUG FIX #4: il codec nel TOC deve riflettere il codec REALMENTE usato
        Codec selected_codec = CodecSelector::select(disk_path, fsize);
        if (selected_codec != Codec::STORE && fsize > STORE_THRESHOLD) {
            fe.meta.codec = static_cast<uint8_t>(Codec::LZMA);
        } else {
            fe.meta.codec = static_cast<uint8_t>(Codec::STORE);
        }
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

            if (fsize <= STORE_THRESHOLD) {
                // BUG FIX #1: file STORE scritti direttamente, NON accodati nel solid_buf
                ChunkResult cr;
                cr.compressed_data = data;
                cr.raw_size = static_cast<uint32_t>(fsize);
                cr.codec = Codec::STORE;
                cr.success = true;

                write_chunk(fg.get(), cr.codec, cr.raw_size, cr.compressed_data, res.bytes_out);
                g_stats.bytes_read += fsize;
            } else if (!opts.solid_mode) {
                // FIX 4a: Non-solid mode — compress each file individually
                // NOTE: data is moved into compress_worker when possible to avoid copies
                ChunkResult cr = compress_worker(std::move(data), level, selected_codec);
                if (!cr.success) {
                    report_warning("Compression failed: " + disk_path);
                    continue;
                }
                write_chunk(fg.get(), cr.codec, cr.raw_size, cr.compressed_data, res.bytes_out);
                g_stats.bytes_read += fsize;
            } else {
                // Solid mode: accumulate files into solid_buf until threshold
                if (solid_buf.size() + fsize > chunk_threshold && !solid_buf.empty()) {
                    // Flush current solid buffer asynchronously
                    if (worker_active && !write_worker(future_chunk)) {
                        res.error = TarcError::CompressionFailed;
                        res.message = "Chunk compression failed.";
                        return res;
                    }
                    // NOTE: solid_buf is moved into std::async — ownership transferred
                    future_chunk = std::async(
                        std::launch::async,
                        compress_worker,
                        std::move(solid_buf),
                        level,
                        Codec::LZMA
                    );
                    worker_active = true;
                    solid_buf.clear();
                    solid_buf.reserve(chunk_threshold);
                    solid_buf.insert(solid_buf.end(), data.begin(), data.end());
                    g_stats.bytes_read += fsize;
                } else {
                    solid_buf.insert(solid_buf.end(), data.begin(), data.end());
                    g_stats.bytes_read += fsize;
                }
            }
        }

        final_toc.push_back(fe);
        g_stats.files_processed++;
    }

    // Wait for any pending async compression worker
    if (worker_active && !write_worker(future_chunk)) {
        res.error = TarcError::CompressionFailed;
        res.message = "Final chunk failed.";
        return res;
    }

    // Flush remaining solid buffer (only exists in solid mode)
    if (!solid_buf.empty()) {
        ChunkResult last = compress_worker(std::move(solid_buf), level, Codec::LZMA);
        write_chunk(fg.get(), last.codec, last.raw_size, last.compressed_data, res.bytes_out);
    }

    ChunkHeader end_mark = {0, 0, 0, 0};
    fwrite(&end_mark, sizeof(end_mark), 1, fg.get());
    IO::write_toc(fg.get(), h, final_toc);
    fflush(fg.get());
    // ARCH-010: FileGuard destructor handles fclose

    // FIX 6: bytes_in accuracy — use g_stats.bytes_read directly, set bytes_compressed
    g_stats.bytes_out = res.bytes_out;
    g_stats.bytes_compressed = res.bytes_out;
    g_stats.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        safe_now() - start_time
    );

    res.ok = true;
    res.bytes_in = g_stats.bytes_read;
    res.bytes_out = res.bytes_out;
    res.message = "Compression completed.";
    return res;
}

static bool match_pattern_impl(const std::string& target, const std::string& pattern, size_t ti, size_t pi) {
    while (ti < target.size() && pi < pattern.size()) {
        if (pattern[pi] == '*') {
            while (pi < pattern.size() && pattern[pi] == '*') pi++;
            if (pi == pattern.size()) return true;

            for (size_t k = ti; k <= target.size(); ++k) {
                if (match_pattern_impl(target, pattern, k, pi)) return true;
            }
            return false;
        } else if (pattern[pi] == '?') {
            ti++;
            pi++;
        } else {
            if (target[ti] != pattern[pi]) return false;
            ti++;
            pi++;
        }
    }

    while (pi < pattern.size() && pattern[pi] == '*') pi++;

    return ti == target.size() && pi == pattern.size();
}

static bool match_pattern(const std::string& full_path, const std::string& pattern) {
    if (pattern.empty()) return true;

    std::string target = full_path;
    if (pattern.find('/') == std::string::npos && pattern.find('\\') == std::string::npos) {
        target = fs::path(full_path).filename().string();
    }

    bool has_wildcard = (pattern.find('*') != std::string::npos || pattern.find('?') != std::string::npos);

    if (!has_wildcard) {
        return target == pattern;
    }

    if (pattern.find("**/") == 0) {
        std::string sub_pattern = pattern.substr(3);
        return match_pattern_impl(full_path, sub_pattern, 0, 0);
    }

    return match_pattern_impl(target, pattern, 0, 0);
}

// ARCH-006: Use ExtractOptions struct in API
TarcResult extract(const std::string& arch_path, const std::vector<std::string>& patterns,
                   const ExtractOptions& opts) {
    bool test_only = opts.test_only;
    size_t offset = 0;
    bool flat_mode = opts.flat_mode;
    bool overwrite = opts.overwrite;

    TarcResult res;
    res.ok = false;
    reset_stats();

    // ARCH-010: RAII wrapper for FILE*
    FileGuard fg(fopen(arch_path.c_str(), "rb"));
    if (!fg) {
        res.error = TarcError::FileNotFound;
        res.message = "Archive not found.";
        return res;
    }

    if (offset > 0) {
        IO::tarc_fseek(fg.get(), static_cast<int64_t>(offset), SEEK_SET);
    }

    Header h;
    if (fread(&h, sizeof(h), 1, fg.get()) != 1) {
        res.error = TarcError::InvalidHeader;
        res.message = "Invalid header.";
        return res;
    }

    // SEC-001: Validate archive header (magic, version, toc_offset)
    if (!IO::validate_archive_header(h)) {
        // Differentiate between magic and version errors
        if (std::memcmp(h.magic, TARC_MAGIC, 4) != 0) {
            res.error = TarcError::InvalidMagic;
            res.message = "Invalid archive magic. Not a TARC archive.";
        } else {
            res.error = TarcError::InvalidVersion;
            res.message = "Unsupported archive version: " + std::to_string(h.version);
        }
        return res;
    }

    std::vector<FileEntry> toc;
    h.toc_offset += offset;
    if (!IO::read_toc(fg.get(), h, toc)) {
        res.error = TarcError::CorruptedArchive;
        res.message = "Cannot read TOC.";
        return res;
    }

    IO::tarc_fseek(fg.get(), static_cast<int64_t>(offset + sizeof(Header)), SEEK_SET);

    std::vector<char> current_block;
    size_t block_pos = 0;
    std::map<std::string, int> flat_names_counter;

    for (size_t i = 0; i < toc.size(); ++i) {
        if (check_cancelled()) {
            res.error = TarcError::Cancelled;
            res.message = "Cancelled.";
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
                if (fread(&ch, sizeof(ch), 1, fg.get()) != 1 || ch.raw_size == 0) break;

                // SEC-004: Anti-OOM chunk size validation
                if (ch.comp_size > TARC_MAX_CHUNK_SIZE || ch.raw_size > TARC_MAX_CHUNK_SIZE) {
                    res.error = TarcError::CorruptedArchive;
                    res.message = "Chunk too large (possible corruption).";
                    return res;
                }

                std::vector<char> comp(ch.comp_size);
                if (fread(comp.data(), 1, ch.comp_size, fg.get()) != ch.comp_size) {
                    res.error = TarcError::CorruptedArchive;
                    res.message = "Error reading chunk.";
                    return res;
                }

                // ARCH-008: Verify chunk checksum (skip section)
                if (ch.checksum != 0) {
                    uint64_t computed = XXH64(comp.data(), comp.size(), 0);
                    if (computed != ch.checksum) {
                        res.error = TarcError::IntegrityCheckFailed;
                        res.message = "Chunk checksum mismatch (data corruption).";
                        return res;
                    }
                }

                current_block.resize(ch.raw_size);

                Codec codec = static_cast<Codec>(ch.codec);
                if (!decompress_chunk(comp, current_block, codec)) {
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
            if (fread(&ch, sizeof(ch), 1, fg.get()) != 1 || ch.raw_size == 0) break;

            // SEC-004: Anti-OOM chunk size validation
            if (ch.comp_size > TARC_MAX_CHUNK_SIZE || ch.raw_size > TARC_MAX_CHUNK_SIZE) {
                res.error = TarcError::CorruptedArchive;
                res.message = "Chunk too large (possible corruption).";
                return res;
            }

            std::vector<char> comp(ch.comp_size);
            if (fread(comp.data(), 1, ch.comp_size, fg.get()) != ch.comp_size) {
                res.error = TarcError::CorruptedArchive;
                res.message = "Error reading data.";
                return res;
            }

            // ARCH-008: Verify chunk checksum (extract section)
            if (ch.checksum != 0) {
                uint64_t computed = XXH64(comp.data(), comp.size(), 0);
                if (computed != ch.checksum) {
                    res.error = TarcError::IntegrityCheckFailed;
                    res.message = "Chunk checksum mismatch (data corruption).";
                    return res;
                }
            }

            current_block.resize(ch.raw_size);

            Codec codec = static_cast<Codec>(ch.codec);
            if (!decompress_chunk(comp, current_block, codec)) {
                res.error = TarcError::DecompressionFailed;
                res.message = "Decompression failed.";
                return res;
            }
            block_pos = 0;
        }

        // SEC-002: Sanitize extraction path (zip-slip / path traversal protection)
        std::string safe_path = IO::sanitize_extract_path(fe.name);
        if (safe_path.empty()) {
            report_warning("Path traversal blocked: " + fe.name);
            res.error = TarcError::PathTraversalDetected;
            res.message = "Path traversal detected in: " + fe.name;
            return res;
        }

        std::string final_path = safe_path;
        if (flat_mode) {
            fs::path p(safe_path);
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

        // SEC-007: Overwrite protection
        if (!test_only && !overwrite && IO::file_exists(final_path)) {
            report_warning("File exists (skipped, use --force to overwrite): " + final_path);
            block_pos += fe.meta.orig_size;
            continue;
        }

        if (!test_only) {
            if (!IO::write_file_to_disk(final_path, current_block.data() + block_pos,
                                   static_cast<size_t>(fe.meta.orig_size),
                                   fe.meta.timestamp, overwrite)) {
                res.error = TarcError::AccessDenied;
                res.message = "Failed to write: " + final_path;
                return res;
            }

            // SEC-006: XXH64 integrity check after extraction
            XXH64_state_t* const verify_state = XXH64_createState();
            if (verify_state) {
                XXH64_reset(verify_state, 0);
                XXH64_update(verify_state,
                            current_block.data() + block_pos,
                            static_cast<size_t>(fe.meta.orig_size));
                uint64_t computed_hash = XXH64_digest(verify_state);
                XXH64_freeState(verify_state);

                if (computed_hash != fe.meta.xxhash) {
                    report_warning("Integrity mismatch: " + final_path);
                    // Non bloccante: avvisa ma continua
                }
            }
        }

        res.bytes_out += fe.meta.orig_size;
        block_pos += fe.meta.orig_size;
        g_stats.files_processed++;
    }

    // ARCH-010: FileGuard destructor handles fclose
    res.ok = true;
    res.message = test_only ? "Test completed." : "Extraction completed.";
    return res;
}

TarcResult list(const std::string& arch_path, size_t offset) {
    TarcResult res;
    res.ok = false;

    // ARCH-010: RAII wrapper for FILE*
    FileGuard fg(fopen(arch_path.c_str(), "rb"));
    if (!fg) {
        res.error = TarcError::FileNotFound;
        res.message = "Archive not found.";
        return res;
    }

    if (offset > 0) {
        IO::tarc_fseek(fg.get(), static_cast<int64_t>(offset), SEEK_SET);
    }

    Header h;
    if (fread(&h, sizeof(h), 1, fg.get()) != 1) {
        res.error = TarcError::InvalidHeader;
        res.message = "Invalid header.";
        return res;
    }

    // SEC-001: Validate archive header
    if (!IO::validate_archive_header(h)) {
        if (std::memcmp(h.magic, TARC_MAGIC, 4) != 0) {
            res.error = TarcError::InvalidMagic;
            res.message = "Invalid archive magic. Not a TARC archive.";
        } else {
            res.error = TarcError::InvalidVersion;
            res.message = "Unsupported archive version: " + std::to_string(h.version);
        }
        return res;
    }

    std::vector<FileEntry> toc;
    h.toc_offset += offset;
    if (!IO::read_toc(fg.get(), h, toc)) {
        res.error = TarcError::CorruptedArchive;
        res.message = "Cannot read TOC.";
        return res;
    }

    // ARCH-012: Return entries in result instead of calling UI directly
    res.entries = std::move(toc);

    // ARCH-010: FileGuard destructor handles fclose
    res.ok = true;
    res.message = "Listed " + std::to_string(res.entries.size()) + " files.";
    return res;
}

// FIX 2 (ARCH-014): remove_files stub removed — it was a non-functional stub

} // namespace Engine
