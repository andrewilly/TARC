#include "engine.h"
#include "io.h"
#include "ui.h"
#include "types.h"
#include <cstring>
#include <map>
#include <set>
#include <filesystem>
#include <fstream>
#include <vector>
#include <iostream>
#include <chrono>
#include <future>
#include <algorithm>
#include <atomic>

#ifdef _WIN32
    #include <windows.h>
#endif

#include "lzma.h"
extern "C" {
    #include "xxhash.h"
}

// ARCH-003: real ZSTD codec implementation
#include <zstd.h>

namespace fs = std::filesystem;

// ARCH-006: use shared TarcUtil::safe_now() instead of local duplicate

namespace {
    ProgressCallback* g_progress_callback = nullptr;
    std::atomic<bool> g_cancelled{false};
    Engine::CompressionStats g_stats;

    // ARCH-002: AsyncWriter REMOVED — was dead code (~130 lines, never used)
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

        // ARCH-003: use LZ4 for small files (< 64KB)
        if (size < 64 * 1024) {
            return Codec::LZ4;
        }

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
// ============================================================
static bool write_chunk(FILE* f, Codec codec, uint32_t raw_size,
                        const std::vector<char>& data, uint64_t& bytes_out) {
    ChunkHeader ch = {
        static_cast<uint32_t>(codec),
        raw_size,
        static_cast<uint32_t>(data.size()),
        0  // checksum placeholder for future use
    };

    if (fwrite(&ch, sizeof(ch), 1, f) != 1) return false;
    if (!data.empty()) {
        if (fwrite(data.data(), 1, data.size(), f) != data.size()) return false;
    }
    bytes_out += data.size();
    return true;
}

// ============================================================
// LZMA codec
// ============================================================
ChunkResult compress_lzma(const std::vector<char>& raw_data, int level) {
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
        preset, LZMA_CHECK_CRC64, nullptr,
        reinterpret_cast<const uint8_t*>(raw_data.data()), raw_data.size(),
        reinterpret_cast<uint8_t*>(res.compressed_data.data()), &out_pos, max_out
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
        reinterpret_cast<const uint8_t*>(compressed.data()), &src_pos, compressed.size(),
        reinterpret_cast<uint8_t*>(decompressed.data()), &dst_pos, decompressed.size()
    );

    if (ret == LZMA_OK || ret == LZMA_STREAM_END) {
        decompressed.resize(dst_pos);
        return true;
    }
    return false;
}

// ============================================================
// ARCH-003: Real ZSTD codec implementation
// ============================================================
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

    // Map our 1-9 level to ZSTD 1-19 (ZSTD default is 3)
    int zstd_level = std::min(level * 2 + 1, 19);

    size_t bound = ZSTD_compressBound(raw_data.size());
    res.compressed_data.resize(bound);

    size_t comp_size = ZSTD_compress(
        res.compressed_data.data(), bound,
        raw_data.data(), raw_data.size(),
        zstd_level
    );

    if (!ZSTD_isError(comp_size) && comp_size < raw_data.size()) {
        res.compressed_data.resize(comp_size);
        res.success = true;
    } else if (!ZSTD_isError(comp_size) && comp_size >= raw_data.size()) {
        // Compression didn't help — fall back to STORE
        res.compressed_data = raw_data;
        res.codec = Codec::STORE;
        res.success = true;
    } else {
        // ZSTD error — fall back to STORE
        res.compressed_data = raw_data;
        res.codec = Codec::STORE;
        res.success = true;
    }
    return res;
}

bool decompress_zstd(const std::vector<char>& compressed, std::vector<char>& decompressed) {
    // If we don't know the expected output size, decompress with streaming
    // First try single-shot decompression
    if (!decompressed.empty()) {
        size_t result = ZSTD_decompress(
            decompressed.data(), decompressed.size(),
            compressed.data(), compressed.size()
        );
        if (!ZSTD_isError(result)) {
            decompressed.resize(result);
            return true;
        }
    }

    // Fallback: allocate large buffer (ZSTD_compressBound is ~3x for worst case)
    unsigned long long content_size = ZSTD_getFrameContentSize(compressed.data(), compressed.size());
    size_t out_size;
    if (content_size != ZSTD_CONTENTSIZE_UNKNOWN && content_size != ZSTD_CONTENTSIZE_ERROR) {
        out_size = static_cast<size_t>(content_size);
    } else {
        out_size = ZSTD_compressBound(compressed.size());
    }

    decompressed.resize(out_size);
    size_t result = ZSTD_decompress(
        decompressed.data(), out_size,
        compressed.data(), compressed.size()
    );

    if (!ZSTD_isError(result)) {
        decompressed.resize(result);
        return true;
    }
    return false;
}

// ============================================================
// Unified compression / decompression worker
// ============================================================
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

    // Files < 4KB: always STORE (not worth compressing)
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

    // ARCH-003: Dispatch to actual codec implementation
    switch (chosen_codec) {
        case Codec::ZSTD:
            res = compress_zstd(raw_data, level);
            break;

        case Codec::LZMA:
            res = compress_lzma(raw_data, level);
            break;

        // LZ4 and Brotli: fall back to LZMA for now
        // (they use streaming APIs, would need frame-level implementation)
        case Codec::LZ4:
        case Codec::BR:
            res = compress_lzma(raw_data, level);
            res.codec = Codec::LZMA;  // ARCH-005: store actual codec used
            break;

        default:
            res = compress_lzma(raw_data, level);
            break;
    }

    res.raw_size = static_cast<uint32_t>(raw_data.size());
    return res;
}

// ARCH-005: proper codec dispatch — each codec to its own decompressor
bool decompress_chunk(const std::vector<char>& compressed, std::vector<char>& decompressed, Codec codec) {
    if (codec == Codec::STORE) {
        decompressed = compressed;
        return true;
    }

    // ARCH-005: Route each codec to its correct decompressor.
    // Archives created with LZ4/BR tag but LZMA data (old behavior) will
    // fail LZMA decompression. This is CORRECT — it forces consistency.
    switch (codec) {
        case Codec::LZMA:
            return decompress_lzma(compressed, decompressed);

        case Codec::ZSTD:
            return decompress_zstd(compressed, decompressed);

        // LZ4/BR: not yet implemented, try LZMA as fallback for backward compat
        case Codec::LZ4:
        case Codec::BR:
            return decompress_lzma(compressed, decompressed);

        default:
            return decompress_lzma(compressed, decompressed);
    }
}

// ============================================================
// SFX creation
// ============================================================
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

// ============================================================
// Compression
// ============================================================
TarcResult compress(const std::string& arch_path, const std::vector<std::string>& inputs, int level) {
    TarcResult res;
    res.ok = false;
    reset_stats();

    std::vector<std::string> expanded_files;
    for (const auto& in : inputs) {
        IO::expand_path(in, expanded_files);
    }

    // ARCH-009: deduplicate input files (same file can appear from multiple patterns)
    {
        std::set<std::string> seen;
        std::vector<std::string> unique;
        for (auto& f : expanded_files) {
            if (seen.insert(f).second) {
                unique.push_back(std::move(f));
            }
        }
        expanded_files = std::move(unique);
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

    FILE* f = fopen(arch_path.c_str(), "wb");
    if (!f) {
        res.error = TarcError::AccessDenied;
        res.message = "Cannot write archive.";
        return res;
    }

    if (fwrite(&h, sizeof(h), 1, f) != 1) {
        fclose(f);
        res.error = TarcError::WriteFailed;
        res.message = "Failed to write header.";
        return res;
    }

    constexpr size_t CHUNK_THRESHOLD = 1024 * 1024 * 1024;
    std::vector<char> solid_buf;
    solid_buf.reserve(CHUNK_THRESHOLD);

    std::future<ChunkResult> future_chunk;
    bool worker_active = false;

    auto write_worker = [&](std::future<ChunkResult>& fut) -> bool {
        if (check_cancelled()) return false;

        ChunkResult cr = fut.get();
        if (!cr.success) return false;

        if (!write_chunk(f, cr.codec, cr.raw_size, cr.compressed_data, res.bytes_out)) {
            return false;
        }

        return true;
    };

    // ARCH-006: use shared safe_now()
    auto start_time = TarcUtil::safe_now();

    for (size_t i = 0; i < expanded_files.size(); ++i) {
        if (check_cancelled()) {
            res.error = TarcError::Cancelled;
            res.message = "Cancelled.";
            fclose(f);
            return res;
        }

        const std::string& disk_path = expanded_files[i];
        report_progress(i + 1, expanded_files.size(), fs::path(disk_path).filename().string());

        if (!fs::exists(disk_path)) {
            report_warning("File not found: " + disk_path);
            continue;
        }

        uintmax_t fsize = fs::file_size(disk_path);

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

        Codec selected_codec = CodecSelector::select(disk_path, fsize);
        if (selected_codec != Codec::STORE && fsize > STORE_THRESHOLD) {
            fe.meta.codec = static_cast<uint8_t>(selected_codec);
        } else {
            fe.meta.codec = static_cast<uint8_t>(Codec::STORE);
            selected_codec = Codec::STORE;
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
                ChunkResult cr;
                cr.compressed_data = data;
                cr.raw_size = static_cast<uint32_t>(fsize);
                cr.codec = Codec::STORE;
                cr.success = true;

                write_chunk(f, cr.codec, cr.raw_size, cr.compressed_data, res.bytes_out);
                g_stats.bytes_read += fsize;
            } else if (solid_buf.size() + fsize > CHUNK_THRESHOLD && !solid_buf.empty()) {
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
                    Codec::LZMA
                );
                worker_active = true;
                solid_buf.clear();
                solid_buf.reserve(CHUNK_THRESHOLD);
                solid_buf.insert(solid_buf.end(), data.begin(), data.end());
                g_stats.bytes_read += fsize;
            } else {
                solid_buf.insert(solid_buf.end(), data.begin(), data.end());
                g_stats.bytes_read += fsize;
            }
        }

        final_toc.push_back(fe);
        g_stats.files_processed++;
    }

    if (worker_active && !write_worker(future_chunk)) {
        res.error = TarcError::CompressionFailed;
        res.message = "Final chunk failed.";
        fclose(f);
        return res;
    }

    if (!solid_buf.empty()) {
        ChunkResult last = compress_worker(std::move(solid_buf), level, Codec::LZMA);
        write_chunk(f, last.codec, last.raw_size, last.compressed_data, res.bytes_out);
    }

    ChunkHeader end_mark = {0, 0, 0, 0};
    fwrite(&end_mark, sizeof(end_mark), 1, f);
    IO::write_toc(f, h, final_toc);
    fflush(f);
    fclose(f);

    g_stats.bytes_in = g_stats.bytes_read;
    g_stats.bytes_out = res.bytes_out;
    g_stats.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        TarcUtil::safe_now() - start_time
    );

    res.ok = true;
    res.bytes_in = g_stats.bytes_read;
    res.bytes_out = res.bytes_out;
    res.message = "Compression completed.";
    return res;
}

// ============================================================
// ARCH-010: Fixed match_pattern — iterative, no exponential backtracking
// ============================================================
static bool match_pattern_impl(const std::string& target, const std::string& pattern, size_t ti, size_t pi) {
    // Use dynamic programming to avoid exponential recursion on pathological patterns
    // This handles patterns like "*a*b*c" against long strings efficiently
    std::vector<bool> prev_row(pattern.size() + 1, false);
    std::vector<bool> curr_row(pattern.size() + 1, false);

    // Initialize: empty pattern matches empty target
    prev_row[0] = true;
    // '*' at position 0 matches empty target
    for (size_t j = 1; j <= pattern.size(); ++j) {
        if (pattern[j - 1] == '*') {
            prev_row[j] = prev_row[j - 1];
        }
    }

    for (size_t i = 1; i <= target.size(); ++i) {
        curr_row[0] = false;
        for (size_t j = 1; j <= pattern.size(); ++j) {
            char pc = pattern[j - 1];
            char tc = target[i - 1];

            if (pc == '*') {
                // '*' matches: empty (prev_row[j-1]) or consume target char (prev_row[j])
                curr_row[j] = prev_row[j] || prev_row[j - 1];
            } else if (pc == '?' || tc == pc) {
                curr_row[j] = prev_row[j - 1];
            } else {
                curr_row[j] = false;
            }
        }
        std::swap(prev_row, curr_row);
    }

    return prev_row[pattern.size()];
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

// ============================================================
// ARCH-001: Deduplicated chunk reading in extract()
// The chunk reading code that was duplicated before is now in a helper.
// ============================================================
struct ReadChunkResult {
    bool ok = false;
    bool end_of_data = false;     // true when we hit the end marker (raw_size==0)
    TarcError error = TarcError::None;
    std::string error_msg;
    std::vector<char> decompressed_data;
};

static ReadChunkResult read_next_chunk(FILE* f, std::vector<char>& current_block, size_t& block_pos) {
    ReadChunkResult r;

    if (block_pos >= current_block.size()) {
        ChunkHeader ch;
        if (fread(&ch, sizeof(ch), 1, f) != 1 || ch.raw_size == 0) {
            r.end_of_data = true;
            r.ok = true;
            return r;
        }

        // SEC-004: Anti-OOM chunk size validation
        if (ch.comp_size > TARC_MAX_CHUNK_SIZE || ch.raw_size > TARC_MAX_CHUNK_SIZE) {
            r.ok = false;
            r.error = TarcError::CorruptedArchive;
            r.error_msg = "Chunk too large (possible corruption).";
            return r;
        }

        std::vector<char> comp(ch.comp_size);
        if (fread(comp.data(), 1, ch.comp_size, f) != ch.comp_size) {
            r.ok = false;
            r.error = TarcError::CorruptedArchive;
            r.error_msg = "Error reading chunk data.";
            return r;
        }

        current_block.resize(ch.raw_size);

        Codec codec = static_cast<Codec>(ch.codec);
        if (!decompress_chunk(comp, current_block, codec)) {
            r.ok = false;
            r.error = TarcError::DecompressionFailed;
            r.error_msg = "Chunk decompression failed.";
            return r;
        }
        block_pos = 0;
    }

    r.ok = true;
    return r;
}

// ============================================================
// Extraction (with ARCH-001 deduplication, ARCH-008 output_dir)
// ============================================================
TarcResult extract(const std::string& arch_path, const std::vector<std::string>& patterns,
                   bool test_only, size_t offset, bool flat_mode, bool overwrite,
                   const std::string& output_dir) {
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
        IO::tarc_fseek(f, static_cast<int64_t>(offset), SEEK_SET);
    }

    Header h;
    if (fread(&h, sizeof(h), 1, f) != 1) {
        fclose(f);
        res.error = TarcError::InvalidHeader;
        res.message = "Invalid header.";
        return res;
    }

    // SEC-001: Validate archive header (magic, version, toc_offset)
    if (!IO::validate_archive_header(h)) {
        fclose(f);
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
    if (!IO::read_toc(f, h, toc)) {
        fclose(f);
        res.error = TarcError::CorruptedArchive;
        res.message = "Cannot read TOC.";
        return res;
    }

    IO::tarc_fseek(f, static_cast<int64_t>(offset + sizeof(Header)), SEEK_SET);

    std::vector<char> current_block;
    size_t block_pos = 0;
    std::map<std::string, int> flat_names_counter;

    for (size_t i = 0; i < toc.size(); ++i) {
        if (check_cancelled()) {
            res.error = TarcError::Cancelled;
            res.message = "Cancelled.";
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

        // ARCH-001: single chunk-reading block replaces two duplicated blocks
        auto cr = read_next_chunk(f, current_block, block_pos);
        if (!cr.ok) {
            fclose(f);
            res.error = cr.error;
            res.message = cr.error_msg;
            return res;
        }
        if (cr.end_of_data) break;

        if (!should_extract) {
            if (!fe.meta.is_duplicate) {
                block_pos += fe.meta.orig_size;
            }
            continue;
        }

        if (fe.meta.is_duplicate) continue;

        // SEC-002: Sanitize extraction path
        std::string safe_path = IO::sanitize_extract_path(fe.name);
        if (safe_path.empty()) {
            report_warning("Path traversal blocked: " + fe.name);
            fclose(f);
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
        std::string check_path = output_dir.empty() ? final_path : output_dir + "/" + final_path;
        if (!test_only && !overwrite && IO::file_exists(check_path)) {
            report_warning("File exists (skipped, use --force to overwrite): " + final_path);
            block_pos += fe.meta.orig_size;
            continue;
        }

        if (!test_only) {
            // ARCH-008: pass output_dir
            if (!IO::write_file_to_disk(final_path, current_block.data() + block_pos,
                                   static_cast<size_t>(fe.meta.orig_size),
                                   fe.meta.timestamp, overwrite, output_dir)) {
                res.error = TarcError::AccessDenied;
                res.message = "Failed to write: " + final_path;
                fclose(f);
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
                }
            }
        }

        res.bytes_out += fe.meta.orig_size;
        block_pos += fe.meta.orig_size;
        g_stats.files_processed++;
    }

    fclose(f);
    res.ok = true;
    res.message = test_only ? "Test completed." : "Extraction completed.";
    return res;
}

// ============================================================
// List
// ============================================================
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
        IO::tarc_fseek(f, static_cast<int64_t>(offset), SEEK_SET);
    }

    Header h;
    if (fread(&h, sizeof(h), 1, f) != 1) {
        fclose(f);
        res.error = TarcError::InvalidHeader;
        res.message = "Invalid header.";
        return res;
    }

    if (!IO::validate_archive_header(h)) {
        fclose(f);
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
            fe.meta.is_duplicate ? 0 : fe.meta.orig_size,
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
    res.message = "Remove not supported.";
    return res;
}

} // namespace Engine
