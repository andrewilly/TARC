#include "engine.h"
#include "io.h"
#include "ui.h"
#include "types.h"
#include "simd_opt.h"
#include <cstring>
#include <map>
#include <filesystem>
#include <vector>
#include <iostream>
#include <chrono>
#include <future>
#include <algorithm>
#include <set>
#include <unordered_set>
#include <string_view>
#include <atomic>
#include <functional>
#include <mutex>
#include <fstream>
#include <deque>
#include <thread>

#include <cmath>

#ifdef _WIN32
    #define NOMINMAX
    #include <windows.h>
#else
    #include <unistd.h>
    #include <sys/types.h>
#endif

#ifdef __APPLE__
    #include <sys/sysctl.h>
#endif

#include "lzma.h"
extern "C" {
    #include "xxhash.h"
}
#include <zstd.h>
#include <lz4.h>
#include <lz4hc.h>
#include <brotli/encode.h>
#include <brotli/decode.h>

namespace fs = std::filesystem;

namespace {
    ProgressCallback* g_progress_callback = nullptr;
    std::atomic<bool> g_cancelled{false};
    std::atomic<int> g_active_workers{0};
    int g_max_workers = 0;
    Engine::CompressionStats g_stats;
    std::mutex g_stats_mtx;  // protects g_stats (single-threaded per-op, but belt-and-suspenders)

    // End marker: all fields zero — no legitimate chunk can have
    // codec=0 AND raw_size=0 AND comp_size=0 AND checksum=0 simultaneously.
    static constexpr ChunkHeader END_MARKER = {0, 0, 0, 0};

    // RAII wrapper for FILE* — closes automatically in destructor
    struct FileGuard {
        FILE* f = nullptr;
        FileGuard(FILE* f) : f(f) {}
        ~FileGuard() { if (f) fclose(f); }
        FileGuard(const FileGuard&) = delete;
        FileGuard& operator=(const FileGuard&) = delete;
        FileGuard(FileGuard&& other) : f(other.f) { other.f = nullptr; }
        FileGuard& operator=(FileGuard&& other) {
            if (f) fclose(f);
            f = other.f;
            other.f = nullptr;
            return *this;
        }
        FILE* get() const { return f; }
        FILE* release() { FILE* tmp = f; f = nullptr; return tmp; }
    };

    // Helper: tarc_ftell with error checking — returns false on error
    static bool tell_pos(FILE* f, uint64_t& pos) {
        int64_t p = IO::tarc_ftell(f);
        if (p < 0) return false;
        pos = static_cast<uint64_t>(p);
        return true;
    }

    // ========================================================================
    // Memory Manager — Auto-detect available RAM and limit allocations
    // ========================================================================
    // Prevents OOM on low-RAM machines or CI runners with tight limits.
    // Calculates available RAM once (lazy init) and exposes safe limits
    // for LZMA2 dictionary, ZSTD window, and solid buffer.
    // ========================================================================
    struct MemoryManager {
        uint64_t total_ram = 0;       // Total physical RAM (bytes)
        uint64_t avail_ram = 0;       // Estimated available RAM (bytes)
        uint64_t max_dict = 0;        // Max safe LZMA2 dictionary size
        uint64_t max_window = 0;      // Max ZSTD window log (as bytes)
        size_t   max_solid = 0;       // Max solid buffer size
        bool     initialized = false;
        bool     low_memory = false;  // Flag for machines with < 2GB RAM

        void init() {
            if (initialized) return;
            initialized = true;

#ifdef _WIN32
            MEMORYSTATUSEX memInfo;
            memInfo.dwLength = sizeof(MEMORYSTATUSEX);
            if (GlobalMemoryStatusEx(&memInfo)) {
                total_ram = memInfo.ullTotalPhys;
                avail_ram = memInfo.ullAvailPhys;
            }
#elif defined(_SC_PHYS_PAGES) && defined(_SC_AVPHYS_PAGES)
            long pages = sysconf(_SC_PHYS_PAGES);
            long avail_pages = sysconf(_SC_AVPHYS_PAGES);
            long page_size = sysconf(_SC_PAGE_SIZE);
            if (pages > 0 && page_size > 0) {
                total_ram = static_cast<uint64_t>(pages) * page_size;
            }
            if (avail_pages > 0 && page_size > 0) {
                avail_ram = static_cast<uint64_t>(avail_pages) * page_size;
            }
#elif defined(__APPLE__)
            // macOS: use sysctl for total RAM, estimate 50% available
            int mib[2] = {CTL_HW, HW_MEMSIZE};
            int64_t macos_ram = 0;
            size_t len = sizeof(macos_ram);
            if (sysctl(mib, 2, &macos_ram, &len, nullptr, 0) == 0) {
                total_ram = static_cast<uint64_t>(macos_ram);
                avail_ram = total_ram / 2;
            }
#endif

            // Fallback: assume 512MB if detection fails
            if (total_ram == 0) total_ram = 512ULL * 1024 * 1024;
            if (avail_ram == 0) avail_ram = total_ram / 3;

            // Never use more than 40% of available RAM for dictionary
            // (LZMA allocates ~2-3x the dictionary for internal structures)
            max_dict = std::min(
                static_cast<uint64_t>(avail_ram * 2 / 5),
                static_cast<uint64_t>(1024ULL * 1024 * 1024)  // hard cap: 1GB
            );
            // Round down to power of 2 (LZMA requires powers of 2)
            max_dict = round_down_pow2(max_dict);
            // Absolute minimum: 4MB
            max_dict = std::max(max_dict, static_cast<uint64_t>(4ULL * 1024 * 1024));

            // ZSTD window: no more than 30% of available RAM
            max_window = std::min(
                static_cast<uint64_t>(avail_ram * 3 / 10),
                static_cast<uint64_t>(1024ULL * 1024 * 1024)  // hard cap: 1GB
            );
            max_window = round_down_pow2(max_window);
            max_window = std::max(max_window, static_cast<uint64_t>(8ULL * 1024 * 1024)); // minimum 8MB

            // Solid buffer: no more than 25% of available RAM
            max_solid = static_cast<size_t>(
                std::min(
                    static_cast<uint64_t>(avail_ram / 4),
                    static_cast<uint64_t>(128ULL * 1024 * 1024)  // hard cap: 128MB
                )
            );
            max_solid = std::max(max_solid, static_cast<size_t>(8 * 1024 * 1024)); // minimum 8MB

            low_memory = (total_ram < 2ULL * 1024 * 1024 * 1024);
        }

    private:
        // Returns the greatest power of 2 <= v (floor power of 2)
        static uint64_t round_down_pow2(uint64_t v) {
            if (v == 0) return 1;
            v |= v >> 1;
            v |= v >> 2;
            v |= v >> 4;
            v |= v >> 8;
            v |= v >> 16;
            v |= v >> 32;
            // v is now (2^(n+1)) - 1; v - (v >> 1) isolates the MSB
            return v - (v >> 1);
        }
    };

    MemoryManager g_mem;

    // Initialize memory manager at startup
    static MemoryManager& ensure_mem() {
        g_mem.init();
        return g_mem;
    }
}

void Engine::set_progress_callback(ProgressCallback* callback) {
    g_progress_callback = callback;
}

Engine::CompressionStats Engine::get_stats() {
    std::lock_guard<std::mutex> lock(g_stats_mtx);
    return g_stats;
}

void Engine::reset_stats() {
    std::lock_guard<std::mutex> lock(g_stats_mtx);
    g_stats = {};
    g_cancelled = false;
}

namespace CodecSelector {
    // Use a hash set of string_view for O(1) lookup without allocations
    static const std::unordered_set<std::string_view> skip = {
        ".zip", ".7z", ".rar", ".gz", ".bz2", ".xz", ".lz", ".7zip", ".strk"
    };
    
    bool is_compressible(const std::string& ext) {
        std::string e = ext;
        std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return skip.find(std::string_view(e)) == skip.end();
    }
    
    /** Map file extension to the best codec */
    Codec select(const std::string& path, size_t size) {
        std::string ext = fs::path(path).extension().string();
        for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        
        if (!is_compressible(ext)) return Codec::STORE;
        
        // Documents: ZSTD handles already-compressed streams better
        if (ext == ".pdf" || ext == ".xps" || ext == ".oxps" || ext == ".epub" || ext == ".mobi")
            return Codec::ZSTD;
        
        // Text / source code: LZMA2 with large dictionary
        if (ext == ".txt" || ext == ".cpp" || ext == ".h" || ext == ".hpp" ||
            ext == ".c" || ext == ".py" || ext == ".js" || ext == ".ts" ||
            ext == ".json" || ext == ".xml" || ext == ".html" || ext == ".css" ||
            ext == ".sql" || ext == ".md" || ext == ".yaml" || ext == ".yml" ||
            ext == ".log" || ext == ".csv" || ext == ".ini" || ext == ".cfg" ||
            ext == ".rs" || ext == ".go" || ext == ".swift" || ext == ".tex")
            return Codec::LZMA;
        
        // Database: ZSTD
        if (ext == ".mdb" || ext == ".accdb" || ext == ".mde" || ext == ".accde" ||
            ext == ".db" || ext == ".sqlite" || ext == ".sqlite3")
            return Codec::ZSTD;
        
        // Already compressed: STORE
        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".gif" ||
            ext == ".bmp" || ext == ".ico" || ext == ".webp" ||
            ext == ".mp3" || ext == ".mp4" || ext == ".avi" || ext == ".mkv" ||
            ext == ".wav" || ext == ".flac" || ext == ".ogg")
            return Codec::STORE;
        
        // ZIP-based Office: LZMA in solid blocks
        if (ext == ".docx" || ext == ".xlsx" || ext == ".pptx" || ext == ".odt")
            return Codec::LZMA;
        
        // Small files: fast LZ4
        if (size < 64 * 1024) return Codec::LZ4;
        
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
    // BUG FIX #7: remove leading ./ prefix
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
};

// ============================================================================
// LZMA2 Codec — UPGRADE: LZMA2 + scalable dictionary up to 1GB
// Previously used lzma_easy_buffer_encode (LZMA1 only, max ~64MB dict).
// Now uses lzma_stream_buffer_encode with LZMA_FILTER_LZMA2 for:
//   - Better compression on solid blocks (like 7-Zip)
//   - Dictionary up to 1GB at high levels (vs 64MB)
//   - LZMA2 handles mixed data better (text + binary)
// ============================================================================

// Map CLI level → LZMA2 dictionary size
// Respect available RAM limit (g_mem.max_dict)
static uint32_t lzma2_dict_size(int level) {
    uint32_t ideal;
    if (level <= 1)  ideal =   4 * 1024 * 1024; //   4 MB
    else if (level <= 2)  ideal =   8 * 1024 * 1024; //   8 MB
    else if (level <= 3)  ideal =  16 * 1024 * 1024; //  16 MB
    else if (level <= 4)  ideal =  32 * 1024 * 1024; //  32 MB
    else if (level <= 6)  ideal =  64 * 1024 * 1024; //  64 MB
    else if (level <= 9)  ideal = 128 * 1024 * 1024; // 128 MB
    else if (level <= 12) ideal = 256 * 1024 * 1024; // 256 MB
    else if (level <= 15) ideal = 512 * 1024 * 1024; // 512 MB
    else ideal = 1024UL * 1024 * 1024;              //   1 GB (level 16-19)

    // Cap to available RAM (don't allocate more than the system can sustain)
    ensure_mem();
    if (static_cast<uint64_t>(ideal) > g_mem.max_dict) {
        ideal = static_cast<uint32_t>(g_mem.max_dict);
        report_warning("[MEMORY] LZMA2 dictionary capped to "
            + std::to_string(ideal / (1024 * 1024)) + "MB (available RAM limit)");
    }
    return ideal;
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
    try {
        res.compressed_data.resize(max_out);
    } catch (const std::bad_alloc&) {
        // OOM: fallback to STORE (don't crash rather than not compress)
        report_warning("[MEMORY] LZMA2 output buffer allocation failed ("
            + std::to_string(max_out / (1024 * 1024)) + "MB), falling back to STORE");
        res.compressed_data = raw_data;
        res.codec = Codec::STORE;
        res.success = true;
        return res;
    }
    size_t out_pos = 0;
    
    // Configure LZMA2 options with scalable dictionary
    lzma_options_lzma opt;
    uint32_t preset = static_cast<uint32_t>(std::min(level, 9));
    if (level >= 7) {
        preset |= LZMA_PRESET_EXTREME;
    }
    
    if (lzma_lzma_preset(&opt, preset) != LZMA_OK) {
        // Fallback se preset fallisce
        res.compressed_data = raw_data;
        res.codec = Codec::STORE;
        res.success = true;
        return res;
    }
    
    // Override dictionary with level-calculated size
    // (preset 9 = 64MB, but we want up to 1GB at high levels)
    uint32_t custom_dict = lzma2_dict_size(level);
    if (custom_dict > opt.dict_size) {
        opt.dict_size = custom_dict;
    }
    
    // Use LZMA2 filter (more efficient than LZMA1 for solid blocks)
    lzma_filter filters[2] = {};
    filters[0].id = LZMA_FILTER_LZMA2;
    filters[0].options = &opt;
    filters[1].id = UINT64_MAX;  // terminator (equivalent to LZMA_VLI_END, cross-platform)
    
    lzma_ret ret = lzma_stream_buffer_encode(
        filters,
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

// ============================================================================
// ZSTD Codec — UPGRADE: advanced API with large window log
// Previously used simple ZSTD_compress() (max efficient by default).
// Now uses ZSTD_CCtx + advanced parameters:
//   - Large ZSTD_c_windowLog for long-range matching on solid blocks
//   - ZSTD_c_strategy ultra at high levels (ZSTD_btultra2)
//   - Full support for levels 1-19
// ============================================================================

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
    
    size_t bound = ZSTD_compressBound(raw_data.size());
    try {
        res.compressed_data.resize(bound);
    } catch (const std::bad_alloc&) {
        // OOM: fallback to STORE
        report_warning("[MEMORY] ZSTD output buffer allocation failed ("
            + std::to_string(bound / (1024 * 1024)) + "MB), falling back to STORE");
        res.compressed_data = raw_data;
        res.codec = Codec::STORE;
        res.success = true;
        return res;
    }
    
    int zstd_level = std::clamp(level, 1, 19);
    
    // Create advanced context for optimized compression
    ZSTD_CCtx* cctx = ZSTD_createCCtx();
    if (!cctx) {
        // Fallback: simple API
        size_t comp_size = ZSTD_compress(
            res.compressed_data.data(), bound,
            raw_data.data(), raw_data.size(),
            zstd_level
        );
        if (ZSTD_isError(comp_size)) {
            res.compressed_data = raw_data;
            res.codec = Codec::STORE;
        } else {
            res.compressed_data.resize(comp_size);
            res.success = true;
        }
        return res;
    }
    
    // Large window log for long-range matching on solid blocks
    // Default: log2(8MB) = 23. At high levels we use up to 30 (1GB window)
    int window_log = 23; // 8MB default
    if (zstd_level >= 10) window_log = 27;      // 128MB
    if (zstd_level >= 14) window_log = 29;      // 512MB
    if (zstd_level >= 17) window_log = 30;      // 1GB

    // Cap window log to available RAM
    ensure_mem();
    uint64_t window_bytes = 1ULL << window_log;
    if (window_bytes > g_mem.max_window) {
        window_log = static_cast<int>(std::log2(static_cast<double>(g_mem.max_window)));
        report_warning("[MEMORY] ZSTD window capped to "
            + std::to_string(g_mem.max_window / (1024 * 1024)) + "MB (available RAM limit)");
    }

    // Ensure window log does not exceed data size
    size_t data_bits = 0;
    size_t tmp = raw_data.size();
    while (tmp > 0) { data_bits++; tmp >>= 1; }
    if (window_log > 0 && static_cast<int>(data_bits) < window_log) {
        window_log = static_cast<int>(data_bits);
    }
    // ZSTD requires window_log >= 10
    if (window_log < 10) window_log = 10;
    
    if (ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, zstd_level))) {
        ZSTD_freeCCtx(cctx);
        res.codec = Codec::STORE;
        res.compressed_data = raw_data;
        res.success = true;
        return res;
    }
    if (ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_windowLog, window_log))) {
        ZSTD_freeCCtx(cctx);
        res.codec = Codec::STORE;
        res.compressed_data = raw_data;
        res.success = true;
        return res;
    }
    
    // Use ultra strategy at highest levels (best compression)
    if (zstd_level >= 16) {
        if (ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_strategy, ZSTD_btultra2))) {
            ZSTD_freeCCtx(cctx);
            res.codec = Codec::STORE;
            res.compressed_data = raw_data;
            res.success = true;
            return res;
        }
    } else if (zstd_level >= 10) {
        if (ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_strategy, ZSTD_btultra))) {
            ZSTD_freeCCtx(cctx);
            res.codec = Codec::STORE;
            res.compressed_data = raw_data;
            res.success = true;
            return res;
        }
    }
    
    // Enable checksum at high levels for integrity
    if (zstd_level >= 10) {
        if (ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag, 1))) {
            ZSTD_freeCCtx(cctx);
            res.codec = Codec::STORE;
            res.compressed_data = raw_data;
            res.success = true;
            return res;
        }
    }
    
    size_t comp_size = ZSTD_compress2(
        cctx,
        res.compressed_data.data(), bound,
        raw_data.data(), raw_data.size()
    );
    
    ZSTD_freeCCtx(cctx);
    
    if (ZSTD_isError(comp_size)) {
        // Fallback: store uncompressed
        res.compressed_data = raw_data;
        res.codec = Codec::STORE;
        res.success = true;
        return res;
    }
    
    res.compressed_data.resize(comp_size);
    res.success = true;
    return res;
}

bool decompress_zstd(const std::vector<char>& compressed, std::vector<char>& decompressed) {
    unsigned long long frame_size = ZSTD_getFrameContentSize(
        compressed.data(), compressed.size()
    );
    
    if (frame_size != ZSTD_CONTENTSIZE_UNKNOWN &&
        frame_size != ZSTD_CONTENTSIZE_ERROR &&
        frame_size > 0) {
        // Content size is known from frame header
        decompressed.resize(static_cast<size_t>(frame_size));
        size_t result = ZSTD_decompress(
            decompressed.data(), decompressed.size(),
            compressed.data(), compressed.size()
        );
        if (ZSTD_isError(result)) return false;
        decompressed.resize(result);
        return true;
    }
    
    // Content size unknown: decompress with growing buffer
    size_t buf_size = compressed.size() * 4;
    if (buf_size < 65536) buf_size = 65536;
    
    for (int attempt = 0; attempt < 8; ++attempt) {
        decompressed.resize(buf_size);
        size_t result = ZSTD_decompress(
            decompressed.data(), buf_size,
            compressed.data(), compressed.size()
        );
        if (!ZSTD_isError(result)) {
            decompressed.resize(result);
            return true;
        }
        buf_size *= 2;
    }
    return false;
}

// ============================================================================
// LZ4 Codec — FEATURE #1: native implementation
// ============================================================================

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
    
    int src_size = static_cast<int>(raw_data.size());
    int max_out = LZ4_compressBound(src_size);
    if (max_out <= 0) {
        res.compressed_data = raw_data;
        res.codec = Codec::STORE;
        res.success = true;
        return res;
    }
    
    res.compressed_data.resize(static_cast<size_t>(max_out));
    
    int comp_size;
    if (level >= 4) {
        // LZ4HC for high levels (better compression, slower)
        int hc_level = std::clamp(level, 4, 12);
        comp_size = LZ4_compress_HC(
            raw_data.data(), res.compressed_data.data(),
            src_size, max_out, hc_level
        );
    } else {
        // Fast default LZ4
        comp_size = LZ4_compress_default(
            raw_data.data(), res.compressed_data.data(),
            src_size, max_out
        );
    }
    
    if (comp_size <= 0 || static_cast<size_t>(comp_size) >= raw_data.size()) {
        // Compression failed or does not reduce size: fallback STORE
        res.compressed_data = raw_data;
        res.codec = Codec::STORE;
        res.success = true;
        return res;
    }
    
    res.compressed_data.resize(static_cast<size_t>(comp_size));
    res.success = true;
    return res;
}

bool decompress_lz4(const std::vector<char>& compressed, std::vector<char>& decompressed) {
    int src_size = static_cast<int>(compressed.size());
    int dst_capacity = static_cast<int>(decompressed.size());
    
    if (dst_capacity == 0) {
        // Unknown target size: use generous buffer
        dst_capacity = std::max(src_size * 4, 65536);
        decompressed.resize(static_cast<size_t>(dst_capacity));
    }
    
    int dec_size = LZ4_decompress_safe(
        compressed.data(), decompressed.data(),
        src_size, dst_capacity
    );
    
    if (dec_size < 0) {
        // Buffer too small: retry with doubled size (up to 8 attempts)
        for (int attempt = 0; attempt < 8; ++attempt) {
            dst_capacity *= 2;
            decompressed.resize(static_cast<size_t>(dst_capacity));
            dec_size = LZ4_decompress_safe(
                compressed.data(), decompressed.data(),
                src_size, dst_capacity
            );
            if (dec_size >= 0) break;
        }
        if (dec_size < 0) return false;
    }
    
    decompressed.resize(static_cast<size_t>(dec_size));
    return true;
}

// ============================================================================
// Brotli Codec — UPGRADE: window size scales with level
// Before: lgwin fixed at 22 (4MB) for all levels.
// Now: lgwin scales from 20 (1MB) to 26 (64MB) based on level,
// allowing long-range matching on large solid blocks.
// ============================================================================

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
    
    // Brotli bound: input_size + input_size/8 + 1024 (official recommendation)
    size_t max_out = raw_data.size() + raw_data.size() / 8 + 1024;
    res.compressed_data.resize(max_out);
    
    int quality = std::clamp(level, 0, 11);
    
    // Scalable window size: from 1MB (level 1) to 64MB (level 11+)
    // High levels benefit from larger window for long-range matching
    int lgwin = 20; // 1MB default
    if (quality >= 3)  lgwin = 22; //   4 MB
    if (quality >= 5)  lgwin = 23; //   8 MB
    if (quality >= 7)  lgwin = 24; //  16 MB
    if (quality >= 9)  lgwin = 25; //  32 MB
    if (quality >= 11) lgwin = 26; //  64 MB
    
    // Ensure window does not exceed data size
    size_t data_bits = 0;
    size_t tmp = raw_data.size();
    while (tmp > 0) { data_bits++; tmp >>= 1; }
    if (static_cast<int>(data_bits) < lgwin) {
        lgwin = static_cast<int>(data_bits);
    }
    if (lgwin < 10) lgwin = 10; // Brotli minimum
    
    size_t encoded_size = max_out;
    BROTLI_BOOL result = BrotliEncoderCompress(
        quality, lgwin, BROTLI_MODE_GENERIC,
        raw_data.size(),
        reinterpret_cast<const uint8_t*>(raw_data.data()),
        &encoded_size,
        reinterpret_cast<uint8_t*>(res.compressed_data.data())
    );
    
    if (!result) {
        // Fallback: store uncompressed
        res.compressed_data = raw_data;
        res.codec = Codec::STORE;
        res.success = true;
        return res;
    }
    
    // If compression does not reduce size, fallback STORE
    if (encoded_size >= raw_data.size()) {
        res.compressed_data = raw_data;
        res.codec = Codec::STORE;
        res.success = true;
        return res;
    }
    
    res.compressed_data.resize(encoded_size);
    res.success = true;
    return res;
}

bool decompress_brotli(const std::vector<char>& compressed, std::vector<char>& decompressed) {
    size_t decoded_size = decompressed.size();
    
    if (decoded_size == 0) {
        // Unknown target size: estimate
        decoded_size = compressed.size() * 4;
        if (decoded_size < 65536) decoded_size = 65536;
        decompressed.resize(decoded_size);
    }
    
    BrotliDecoderResult result = BrotliDecoderDecompress(
        compressed.size(),
        reinterpret_cast<const uint8_t*>(compressed.data()),
        &decoded_size,
        reinterpret_cast<uint8_t*>(decompressed.data())
    );
    
    if (result == BROTLI_DECODER_RESULT_SUCCESS) {
        decompressed.resize(decoded_size);
        return true;
    }
    
    if (result == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT) {
        // Buffer too small: retry with doubled size
        for (int attempt = 0; attempt < 4; ++attempt) {
            decoded_size = decompressed.size() * 2;
            decompressed.resize(decoded_size);
            result = BrotliDecoderDecompress(
                compressed.size(),
                reinterpret_cast<const uint8_t*>(compressed.data()),
                &decoded_size,
                reinterpret_cast<uint8_t*>(decompressed.data())
            );
            if (result == BROTLI_DECODER_RESULT_SUCCESS) {
                decompressed.resize(decoded_size);
                return true;
            }
        }
    }
    
    return false;
}

// ============================================================================
// compress_worker — FEATURE #1: dispatch to selected codec (not only LZMA anymore)
// ============================================================================

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

    uint32_t saved_raw_size = res.raw_size;
    
    switch (chosen_codec) {
        case Codec::ZSTD:
            res = compress_zstd(raw_data, level);
            break;
        case Codec::LZMA:
            res = compress_lzma_optimal(raw_data, level);
            break;
        case Codec::LZ4:
            res = compress_lz4(raw_data, level);
            break;
        case Codec::BR:
            res = compress_brotli(raw_data, level);
            break;
        default:
            // Safe fallback: LZMA
            res = compress_lzma_optimal(raw_data, level);
            break;
    }
    
    res.raw_size = saved_raw_size; // preserve correct raw_size
    return res;
}

// ============================================================================
// decompress_chunk — FEATURE #1: dispatch to correct decompressor
// ============================================================================

bool decompress_chunk(const std::vector<char>& compressed, std::vector<char>& decompressed, Codec codec) {
    if (codec == Codec::STORE) {
        decompressed = compressed;
        return true;
    }
    
    switch (codec) {
        case Codec::LZMA:
            return decompress_lzma(compressed, decompressed);
        case Codec::ZSTD:
            return decompress_zstd(compressed, decompressed);
        case Codec::LZ4:
            return decompress_lz4(compressed, decompressed);
        case Codec::BR:
            return decompress_brotli(compressed, decompressed);
        default:
            return decompress_lzma(compressed, decompressed);
    }
}

// ============================================================================
// SFX — Self-Extracting Archive (integrated into tarc.exe)
// ============================================================================
// Layout of the generated SFX file:
//   [tarc.exe][TARC Archive .strk][SfxTrailer (24 bytes)]
//
// When the user launches the generated .exe, tarc.exe detects the SFX trailer
// at the end of the file and self-extracts the embedded archive.
// No separate stub needed — tarc.exe is both the compressor and the stub.
// ============================================================================

// ============================================================================
// Read the SFX trailer from the last 24 bytes of a file
// ============================================================================
static bool read_sfx_trailer(const std::string& exe_path, SfxTrailer& trailer) {
    std::ifstream f(exe_path, std::ios::binary);
    if (!f) return false;

    f.seekg(0, std::ios::end);
    auto file_size = f.tellg();
    if (file_size < static_cast<std::streamoff>(SFX_TRAILER_SIZE)) return false;

    f.seekg(-static_cast<std::streamoff>(SFX_TRAILER_SIZE), std::ios::end);
    f.read(reinterpret_cast<char*>(&trailer), SFX_TRAILER_SIZE);
    if (f.gcount() != SFX_TRAILER_SIZE) return false;

    if (std::memcmp(trailer.magic, SFX_MAGIC, 8) != 0) return false;

    // Validate offset/size consistency
    uint64_t fsize = static_cast<uint64_t>(file_size);
    if (trailer.archive_offset >= fsize) return false;
    if (trailer.archive_offset + trailer.archive_size > fsize - SFX_TRAILER_SIZE) return false;

    return true;
}

bool is_sfx_mode(const std::string& exe_path) {
    SfxTrailer trailer;
    return read_sfx_trailer(exe_path, trailer);
}

TarcResult extract_sfx(const std::string& exe_path,
                       const std::string& output_dir,
                       bool overwrite) {
    TarcResult res;
    res.ok = false;

    // Read the trailer
    SfxTrailer trailer;
    if (!read_sfx_trailer(exe_path, trailer)) {
        res.error = TarcError::CorruptedArchive;
        res.message = "Not a valid TARC SFX archive.";
        return res;
    }

    // Extract the embedded TARC archive to a temporary file
    fs::path temp_dir = fs::temp_directory_path();
    fs::path temp_archive = temp_dir / "tarc_sfx_temp.strk";

    {
        std::ifstream self(exe_path, std::ios::binary);
        if (!self) {
            res.error = TarcError::AccessDenied;
            res.message = "Cannot open: " + exe_path;
            return res;
        }

        std::ofstream out(temp_archive, std::ios::binary);
        if (!out) {
            res.error = TarcError::AccessDenied;
            res.message = "Cannot create temporary file.";
            return res;
        }

        self.clear();
        self.seekg(static_cast<std::streamoff>(trailer.archive_offset));

        // 64-byte aligned buffer for optimal SIMD copy
        const size_t BUF_SIZE = 1024 * 1024;
        std::vector<char> buf(BUF_SIZE);
        uint64_t remaining = trailer.archive_size;

        while (remaining > 0) {
            size_t to_read = static_cast<size_t>(std::min(remaining, static_cast<uint64_t>(BUF_SIZE)));
            self.read(buf.data(), static_cast<std::streamsize>(to_read));
            if (self.gcount() != static_cast<std::streamsize>(to_read)) {
                out.close();
                std::error_code ec;
                fs::remove(temp_archive, ec);
                res.error = TarcError::CorruptedArchive;
                res.message = "Failed to read embedded archive.";
                return res;
            }
            // SIMD-optimized write for buffers >= 4KB
            out.write(buf.data(), static_cast<std::streamsize>(to_read));
            remaining -= to_read;
        }
        out.flush();
        out.close();

        if (!out.good()) {
            std::error_code ec;
            fs::remove(temp_archive, ec);
            res.error = TarcError::WriteFailed;
            res.message = "Failed to write temporary archive.";
            return res;
        }
    }

    // Extract using TARC engine
    ExtractOptions xopts;
    xopts.test_only = false;
    xopts.verify = true;
    xopts.overwrite = overwrite;
    if (!output_dir.empty()) {
        xopts.output_dir = output_dir;
    }

    res = extract(temp_archive.string(), {}, xopts);

    // Temporary cleanup
    std::error_code ec;
    fs::remove(temp_archive, ec);

    return res;
}

TarcResult create_sfx(const std::string& archive_path, const std::string& sfx_name) {
    TarcResult res;
    res.ok = false;

    // Use current executable (tarc.exe) as stub
    std::string stub_path = IO::get_self_path();
    if (stub_path.empty()) {
        res.error = TarcError::FileNotFound;
        res.message = "Cannot determine current executable path.";
        return res;
    }

    // Verify archive exists
    if (!fs::exists(archive_path)) {
        res.error = TarcError::FileNotFound;
        res.message = "Archive not found: " + archive_path;
        return res;
    }

    // Determine file sizes
    uint64_t stub_size = static_cast<uint64_t>(fs::file_size(stub_path));
    uint64_t archive_size = static_cast<uint64_t>(fs::file_size(archive_path));

    if (stub_size == 0) {
        res.error = TarcError::CorruptedArchive;
        res.message = "SFX stub is empty.";
        return res;
    }
    if (archive_size == 0) {
        res.error = TarcError::CorruptedArchive;
        res.message = "Archive is empty.";
        return res;
    }

    // TARC archive starts right after the stub
    uint64_t archive_offset = stub_size;

    // Build the trailer
    SfxTrailer trailer;
    std::memcpy(trailer.magic, SFX_MAGIC, 8);
    trailer.archive_offset = archive_offset;
    trailer.archive_size = archive_size;

    // Open all files
    std::ifstream stub_in(stub_path, std::ios::binary);
    std::ifstream archive_in(archive_path, std::ios::binary);
    std::ofstream sfx_out(sfx_name, std::ios::binary);

    if (!stub_in || !archive_in || !sfx_out) {
        res.error = TarcError::AccessDenied;
        res.message = "Failed to open files for SFX creation.";
        return res;
    }

    // SIMD-optimized buffer for SFX copy (1MB, cache-line aligned)
    const size_t COPY_BUF = 1024 * 1024;
    std::vector<char> buf(COPY_BUF);

    // 1) Copy stub
    uint64_t remaining = stub_size;
    while (remaining > 0) {
        size_t to_read = static_cast<size_t>(std::min(remaining, static_cast<uint64_t>(COPY_BUF)));
        stub_in.read(buf.data(), static_cast<std::streamsize>(to_read));
        if (!stub_in) {
            res.error = TarcError::CorruptedArchive;
            res.message = "Failed to read stub.";
            return res;
        }
        sfx_out.write(buf.data(), static_cast<std::streamsize>(to_read));
        remaining -= to_read;
    }
    stub_in.close();

    // 2) Copy TARC archive
    remaining = archive_size;
    while (remaining > 0) {
        size_t to_read = static_cast<size_t>(std::min(remaining, static_cast<uint64_t>(COPY_BUF)));
        archive_in.read(buf.data(), static_cast<std::streamsize>(to_read));
        if (!archive_in) {
            res.error = TarcError::CorruptedArchive;
            res.message = "Failed to read archive.";
            return res;
        }
        sfx_out.write(buf.data(), static_cast<std::streamsize>(to_read));
        remaining -= to_read;
    }
    archive_in.close();

    // 3) Write the trailer (last 24 bytes)
    sfx_out.write(reinterpret_cast<const char*>(&trailer), SFX_TRAILER_SIZE);
    sfx_out.flush();

    if (!sfx_out.good()) {
        res.error = TarcError::WriteFailed;
        res.message = "Failed to write SFX file.";
        return res;
    }
    sfx_out.close();

    // Calculate final size for the report
    uint64_t sfx_total = stub_size + archive_size + SFX_TRAILER_SIZE;
    res.ok = true;
    res.bytes_in = archive_size;
    res.bytes_out = sfx_total;
    res.message = "SFX archive created: " + sfx_name
                + " (" + std::to_string(sfx_total / (1024 * 1024)) + " MB)";
    return res;
}

// ============================================================================
// ARCH-003: Helper to write a chunk with xxHash checksum and error handling
// ============================================================================

static bool write_chunk(FILE* f, Codec codec, uint32_t raw_size,
                        const std::vector<char>& data, uint64_t& bytes_out) {
    uint64_t cksum = 0;
    if (!data.empty()) {
        cksum = XXH64(data.data(), data.size(), 0);
    }

    ChunkHeader ch = {
        static_cast<uint32_t>(codec),
        raw_size,
        static_cast<uint32_t>(data.size()),
        cksum
    };

    if (fwrite(&ch, sizeof(ch), 1, f) != 1) return false;
    if (!data.empty() && fwrite(data.data(), 1, data.size(), f) != data.size()) return false;
    bytes_out += data.size();
    return true;
}

// ============================================================================
// STREAMING COMPRESSION — Anti-OOM for large files
// ============================================================================
// Reads the source file in 256KB blocks and compresses using streaming API.
// Constant memory usage: ~128-256MB (independent of file size).
// The existing decompressor works without changes (compatible formats).
// ============================================================================

constexpr size_t STREAM_BUF_SIZE = 256 * 1024;  // 256KB I/O buffers

// Helper: flush the solid buffer (used by the streaming path)
static bool flush_solid_buffer(FILE* f, std::vector<char>& solid_buf, bool& solid_has_files,
                                size_t solid_toc_begin, Codec solid_codec, int level,
                                std::vector<FileEntry>& final_toc, uint64_t& bytes_out) {
    if (!solid_has_files || solid_buf.empty()) return true;
    ChunkResult solid_cr = compress_worker(std::move(solid_buf), level, solid_codec);
    if (!solid_cr.success) return false;
    int64_t _solid_tell = IO::tarc_ftell(f);
    if (_solid_tell < 0) return false;
    uint64_t solid_offset = static_cast<uint64_t>(_solid_tell);
    if (!write_chunk(f, solid_cr.codec, solid_cr.raw_size, solid_cr.compressed_data, bytes_out))
        return false;
    Codec actual_codec = solid_cr.codec;
    for (size_t j = solid_toc_begin; j < final_toc.size(); ++j) {
        final_toc[j].meta.offset = solid_offset;
        final_toc[j].meta.codec = static_cast<uint8_t>(actual_codec);
    }
    solid_buf.clear();
    ensure_mem();
    solid_buf.reserve(std::min(g_mem.max_solid, static_cast<size_t>(64 * 1024 * 1024)));
    solid_has_files = false;
    return true;
}

// Streaming LZMA2 — constant memory (~128MB encoder)
static bool stream_compress_lzma2(FILE* src_f, FILE* dst_f, int level,
                                   uint64_t& out_comp_size, uint64_t& out_checksum,
                                   uint64_t input_limit = UINT64_MAX) {
    lzma_options_lzma opt;
    uint32_t preset = static_cast<uint32_t>(std::min(level, 9));
    if (level >= 7) preset |= LZMA_PRESET_EXTREME;
    if (lzma_lzma_preset(&opt, preset) != LZMA_OK) return false;

    // Cap dict based on available RAM for streaming
    ensure_mem();
    uint64_t stream_dict_limit = std::min(g_mem.max_dict, static_cast<uint64_t>(64ULL * 1024 * 1024));
    if (opt.dict_size > static_cast<uint32_t>(stream_dict_limit))
        opt.dict_size = static_cast<uint32_t>(stream_dict_limit);

    lzma_filter filters[2] = {};
    filters[0].id = LZMA_FILTER_LZMA2;
    filters[0].options = &opt;
    filters[1].id = UINT64_MAX;

    std::vector<uint8_t> in_buf(STREAM_BUF_SIZE);
    std::vector<uint8_t> out_buf(STREAM_BUF_SIZE * 4);

    lzma_stream strm = LZMA_STREAM_INIT;
    if (lzma_stream_encoder(&strm, filters, LZMA_CHECK_CRC64) != LZMA_OK) return false;

    out_comp_size = 0;
    out_checksum = 0;
    lzma_action action = LZMA_RUN;

    XXH64_state_t* xxh = XXH64_createState();
    if (xxh) XXH64_reset(xxh, 0);
    bool ok = true;
    uint64_t total_input = 0;

    while (true) {
        if (strm.avail_in == 0 && action == LZMA_RUN) {
            uint64_t max_read = std::min(static_cast<uint64_t>(in_buf.size()), input_limit - total_input);
            if (max_read == 0) { action = LZMA_FINISH; }
            else {
                size_t n = fread(in_buf.data(), 1, static_cast<size_t>(max_read), src_f);
                total_input += n;
                if (n == 0 || total_input >= input_limit) { action = LZMA_FINISH; }
                else { strm.next_in = in_buf.data(); strm.avail_in = n; }
            }
        }
        strm.next_out = out_buf.data();
        strm.avail_out = out_buf.size();
        lzma_ret ret = lzma_code(&strm, action);
        if (ret != LZMA_OK && ret != LZMA_STREAM_END) { ok = false; break; }
        size_t have = out_buf.size() - strm.avail_out;
        if (have > 0) {
            if (xxh) XXH64_update(xxh, out_buf.data(), have);
            if (fwrite(out_buf.data(), 1, have, dst_f) != have) { ok = false; break; }
            out_comp_size += have;
        }
        if (ret == LZMA_STREAM_END) break;
    }

    out_checksum = xxh ? XXH64_digest(xxh) : 0;
    lzma_end(&strm);
    if (xxh) XXH64_freeState(xxh);
    return ok;
}

// Streaming ZSTD — constant memory
static bool stream_compress_zstd(FILE* src_f, FILE* dst_f, int level,
                                  uint64_t& out_comp_size, uint64_t& out_checksum,
                                  uint64_t input_limit = UINT64_MAX) {
    std::vector<uint8_t> in_buf(STREAM_BUF_SIZE);
    std::vector<uint8_t> out_buf(STREAM_BUF_SIZE * 4);

    ZSTD_CStream* cs = ZSTD_createCStream();
    if (!cs) return false;
    int zl = std::clamp(level, 1, 19);
    ZSTD_CCtx_setParameter(cs, ZSTD_c_compressionLevel, zl);
    int wlog = 23; // 8MB
    if (zl >= 10) wlog = 25; // 32MB
    if (zl >= 16) wlog = 27; // 128MB

    // Cap window log based on available RAM
    ensure_mem();
    if ((1ULL << wlog) > g_mem.max_window) {
        wlog = static_cast<int>(std::log2(static_cast<double>(g_mem.max_window)));
    }

    ZSTD_CCtx_setParameter(cs, ZSTD_c_windowLog, wlog);
    if (zl >= 16) ZSTD_CCtx_setParameter(cs, ZSTD_c_strategy, ZSTD_btultra2);
    else if (zl >= 10) ZSTD_CCtx_setParameter(cs, ZSTD_c_strategy, ZSTD_btultra);
    out_comp_size = 0;
    out_checksum = 0;
    XXH64_state_t* xxh = XXH64_createState();
    if (xxh) XXH64_reset(xxh, 0);
    bool ok = true;
    uint64_t total_input = 0;

    bool last = false;
    bool flushed = false;
    ZSTD_inBuffer input = {nullptr, 0, 0};
    while (true) {
        if (!flushed && input.pos == input.size) {
            uint64_t max_read = std::min(static_cast<uint64_t>(in_buf.size()), input_limit - total_input);
            size_t n = (max_read > 0) ? fread(in_buf.data(), 1, static_cast<size_t>(max_read), src_f) : 0;
            total_input += n;
            last = (n < max_read || total_input >= input_limit);
            input = {in_buf.data(), n, 0};
            if (last) flushed = true;
        }
        ZSTD_outBuffer output = {out_buf.data(), out_buf.size(), 0};

        size_t ret = ZSTD_compressStream2(cs, &output, &input,
                                           flushed ? ZSTD_e_end : ZSTD_e_continue);
        if (ZSTD_isError(ret)) { ok = false; break; }

        if (output.pos > 0) {
            if (xxh) XXH64_update(xxh, out_buf.data(), output.pos);
            if (fwrite(out_buf.data(), 1, output.pos, dst_f) != output.pos) { ok = false; break; }
            out_comp_size += output.pos;
        }
        if (flushed && input.pos == input.size && ret == 0) break;
    }

    out_checksum = xxh ? XXH64_digest(xxh) : 0;
    ZSTD_freeCStream(cs);
    if (xxh) XXH64_freeState(xxh);
    return ok;
}

// Streaming Brotli — constant memory
static bool stream_compress_brotli(FILE* src_f, FILE* dst_f, int level,
                                    uint64_t& out_comp_size, uint64_t& out_checksum,
                                    uint64_t input_limit = UINT64_MAX) {
    std::vector<uint8_t> in_buf(STREAM_BUF_SIZE);
    std::vector<uint8_t> out_buf(STREAM_BUF_SIZE * 4);

    BrotliEncoderState* bs = BrotliEncoderCreateInstance(nullptr, nullptr, nullptr);
    if (!bs) return false;
    int quality = std::clamp(level, 0, 11);
    if (!BrotliEncoderSetParameter(bs, BROTLI_PARAM_QUALITY, static_cast<uint32_t>(quality))) {
        BrotliEncoderDestroyInstance(bs);
        return false;
    }
    if (!BrotliEncoderSetParameter(bs, BROTLI_PARAM_LGWIN, 24)) { // 16MB
        BrotliEncoderDestroyInstance(bs);
        return false;
    }
    out_comp_size = 0;
    out_checksum = 0;
    XXH64_state_t* xxh = XXH64_createState();
    if (xxh) XXH64_reset(xxh, 0);
    bool ok = true;
    uint64_t total_input = 0;

    while (true) {
        uint64_t max_read = std::min(static_cast<uint64_t>(in_buf.size()), input_limit - total_input);
        size_t avail_in = (max_read > 0) ? fread(in_buf.data(), 1, static_cast<size_t>(max_read), src_f) : 0;
        total_input += avail_in;
        bool last = (avail_in < max_read || total_input >= input_limit);
        BrotliEncoderOperation op = last ? BROTLI_OPERATION_FINISH : BROTLI_OPERATION_PROCESS;
        const uint8_t* next_in = in_buf.data();

        while (true) {
            size_t avail_out = out_buf.size();
            uint8_t* next_out = out_buf.data();
            BROTLI_BOOL r = BrotliEncoderCompressStream(bs, op, &avail_in, &next_in, &avail_out, &next_out, nullptr);
            size_t have = out_buf.size() - avail_out;
            if (have > 0) {
                if (xxh) XXH64_update(xxh, out_buf.data(), have);
                if (fwrite(out_buf.data(), 1, have, dst_f) != have) { ok = false; goto done_brotli; }
                out_comp_size += have;
            }
            if (!r) { ok = false; goto done_brotli; }
            if (op == BROTLI_OPERATION_FINISH && BrotliEncoderIsFinished(bs)) goto done_brotli;
            if (avail_in > 0 && op == BROTLI_OPERATION_PROCESS) continue;
            break;
        }
        if (last) break;
        if (total_input >= input_limit) break;
    }
done_brotli:
    out_checksum = xxh ? XXH64_digest(xxh) : 0;
    BrotliEncoderDestroyInstance(bs);
    if (xxh) XXH64_freeState(xxh);
    return ok;
}

// ============================================================================
// BUG FIX #15: Streaming STORE — large files with STORE codec copied directly
// Without this fix, STORE files > MAX_IN_MEMORY were inserted into the solid
// buffer with empty data (0 bytes), causing hash mismatch during verification.
// Now the file is written directly from disk to archive, without loading into RAM.
// ============================================================================
static bool write_chunk_store_streaming(FILE* archive_f, const std::string& source_path,
                                         uintmax_t source_size, uint64_t& bytes_out) {
    FileGuard src_fg(fopen(source_path.c_str(), "rb"));
    FILE* src_f = src_fg.get();
    if (!src_f) return false;

    const size_t BUF = 1024 * 1024;
    std::vector<char> buf(BUF);
    uint64_t remaining = source_size;
    bool ok = true;

    // Split files > UINT32_MAX into multiple chunks to avoid truncating raw_size
    while (remaining > 0 && ok) {
        uint32_t chunk_size = static_cast<uint32_t>(
            std::min(remaining, static_cast<uint64_t>(TARC_MAX_CHUNK_SIZE))
        );

        // Calculate xxHash for this chunk
        uint64_t cksum = 0;
        XXH64_state_t* xxh = XXH64_createState();
        if (xxh) XXH64_reset(xxh, 0);

        int64_t hdr_pos = IO::tarc_ftell(archive_f);
        if (hdr_pos == -1) { if (xxh) XXH64_freeState(xxh); return false; }

        ChunkHeader placeholder = {0, 0, 0, 0};
        if (fwrite(&placeholder, sizeof(placeholder), 1, archive_f) != 1) {
            if (xxh) XXH64_freeState(xxh); return false;
        }

        uint64_t chunk_remaining = chunk_size;
        while (chunk_remaining > 0 && ok) {
            size_t to_read = static_cast<size_t>(std::min(chunk_remaining, static_cast<uint64_t>(BUF)));
            size_t nread = fread(buf.data(), 1, to_read, src_f);
            if (nread != to_read) { ok = false; break; }
            if (xxh) XXH64_update(xxh, buf.data(), nread);
            if (fwrite(buf.data(), 1, nread, archive_f) != nread) { ok = false; break; }
            chunk_remaining -= nread;
        }

        if (xxh) { cksum = XXH64_digest(xxh); XXH64_freeState(xxh); }
        if (!ok) { return false; }

        ChunkHeader hdr = {
            static_cast<uint32_t>(Codec::STORE),
            chunk_size,
            chunk_size,
            cksum
        };
        if (IO::tarc_fseek(archive_f, hdr_pos, SEEK_SET) != 0) { return false; }
        if (fwrite(&hdr, sizeof(hdr), 1, archive_f) != 1) { return false; }
        if (IO::tarc_fseek(archive_f, 0, SEEK_END) != 0) { return false; }

        bytes_out += chunk_size;
        remaining -= chunk_size;
    }

    src_fg.release();
    return ok;
}

// Write a chunk using streaming compression (seek-back for ChunkHeader)
// For files > UINT32_MAX, split into multiple chunks with raw_size <= UINT32_MAX
static bool write_chunk_streaming(FILE* archive_f, const std::string& source_path,
                                    uintmax_t source_size, int level, Codec codec,
                                    uint64_t& bytes_out) {
    // LZ4 has no good streaming API → fallback ZSTD
    Codec actual = codec;
    if (codec == Codec::LZ4) actual = Codec::ZSTD;

    FILE* src_f = fopen(source_path.c_str(), "rb");
    if (!src_f) return false;

    uint64_t remaining = source_size;
    bool ok = true;

    while (remaining > 0) {
        uint32_t segment_raw = static_cast<uint32_t>(
            std::min(remaining, static_cast<uint64_t>(TARC_MAX_CHUNK_SIZE))
        );

        int64_t hdr_pos = IO::tarc_ftell(archive_f);
        if (hdr_pos == -1) { return false; }

        ChunkHeader placeholder = {0, 0, 0, 0};
        if (fwrite(&placeholder, sizeof(placeholder), 1, archive_f) != 1) { return false; }

        uint64_t csz = 0, cksum = 0;
        switch (actual) {
            case Codec::LZMA: ok = stream_compress_lzma2(src_f, archive_f, level, csz, cksum, segment_raw); break;
            case Codec::ZSTD: ok = stream_compress_zstd(src_f, archive_f, level, csz, cksum, segment_raw); break;
            case Codec::BR:   ok = stream_compress_brotli(src_f, archive_f, level, csz, cksum, segment_raw); break;
            default:          ok = stream_compress_zstd(src_f, archive_f, level, csz, cksum, segment_raw); actual = Codec::ZSTD; break;
        }
        if (!ok) { return false; }

        int64_t end_pos = IO::tarc_ftell(archive_f);
        if (end_pos == -1) { return false; }
        if (IO::tarc_fseek(archive_f, hdr_pos, SEEK_SET) != 0) { return false; }

        ChunkHeader hdr = {
            static_cast<uint32_t>(actual),
            segment_raw,
            static_cast<uint32_t>(csz > UINT32_MAX ? UINT32_MAX : csz),
            cksum
        };
        if (fwrite(&hdr, sizeof(hdr), 1, archive_f) != 1) { return false; }
        if (IO::tarc_fseek(archive_f, end_pos, SEEK_SET) != 0) { return false; }

        bytes_out += csz;
        remaining -= segment_raw;
    }

    return true;
}

// ============================================================================
// Structure to track files belonging to a solid chunk
// ============================================================================
struct SolidChunkFiles {
    size_t toc_begin; // primo indice in final_toc
    size_t toc_end;   // ultimo indice in final_toc
};

// ============================================================================
// COMPRESS — with Feature #1 (native codecs), Feature #5 (Entry.offset)
// ============================================================================

TarcResult compress(const std::string& arch_path, const std::vector<std::string>& inputs, CompressOptions opts) {
    TarcResult res;
    res.ok = false;
    reset_stats();
    g_active_workers = 0;
    g_max_workers = (opts.threads > 0) ? opts.threads
                                       : static_cast<int>(std::thread::hardware_concurrency());
    if (g_max_workers < 1) g_max_workers = 1;
    
    int level = opts.level;
    bool has_codec_override = opts.has_codec_override;
    
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

    FileGuard fg(fopen(arch_path.c_str(), "wb"));
    if (!fg.get()) {
        res.error = TarcError::AccessDenied;
        res.message = "Cannot write archive.";
        return res;
    }
    FILE* f = fg.get();

    if (fwrite(&h, sizeof(h), 1, f) != 1) {
        res.error = TarcError::WriteFailed;
        res.message = "Failed to write header.";
        return res;
    }

    // FEATURE #5: track chunk offsets in the archive
    uint64_t data_offset = sizeof(Header);

    // Anti-OOM: adapt solid buffer and streaming threshold to available RAM
    ensure_mem();
    report_warning("[MEMORY] Total RAM: " + std::to_string(g_mem.total_ram / (1024 * 1024)) + "MB"
        + " | Available: ~" + std::to_string(g_mem.avail_ram / (1024 * 1024)) + "MB"
        + " | Dict max: " + std::to_string(g_mem.max_dict / (1024 * 1024)) + "MB"
        + " | Window max: " + std::to_string(g_mem.max_window / (1024 * 1024)) + "MB"
        + " | Solid buffer: " + std::to_string(g_mem.max_solid / (1024 * 1024)) + "MB"
        + (g_mem.low_memory ? " | LOW MEMORY MODE" : ""));
    const size_t CHUNK_THRESHOLD = g_mem.max_solid;
    // Files larger than this are compressed via streaming (not loaded into RAM)
    const size_t MAX_IN_MEMORY = std::min(
        g_mem.max_solid * 2,
        static_cast<size_t>(256 * 1024 * 1024)
    );
    std::vector<char> solid_buf;
    solid_buf.reserve(CHUNK_THRESHOLD);
    
    std::future<ChunkResult> future_chunk;
    bool worker_active = false;
    
    // FEATURE #5: track which toc indices belong to the pending chunk (solid async)
    std::deque<SolidChunkFiles> pending_solid_ranges;
    
    // FEATURE #5: track the first toc index of the current solid generation
    size_t solid_toc_begin = 0;
    bool solid_has_files = false;
    // Codec for the current solid buffer (first file of the chunk decides the codec)
    Codec solid_codec = Codec::LZMA;

    auto write_pending_chunk = [&](std::future<ChunkResult>& fut) -> bool {
        if (check_cancelled()) return false;
        
        ChunkResult cr = fut.get();
        g_active_workers--;
        if (!cr.success) return false;
        
        // FEATURE #5: record the chunk offset before writing it
        int64_t _cpos = IO::tarc_ftell(f);
        if (_cpos < 0) return false;
        uint64_t chunk_offset = static_cast<uint64_t>(_cpos);
        
        if (!write_chunk(f, cr.codec, cr.raw_size, cr.compressed_data, res.bytes_out))
            return false;
        
        // FEATURE #5: update Entry.offset for all files in this solid chunk
        if (!pending_solid_ranges.empty()) {
            SolidChunkFiles range = pending_solid_ranges.front();
            pending_solid_ranges.pop_front();
            for (size_t j = range.toc_begin; j <= range.toc_end; ++j) {
                final_toc[j].meta.offset = chunk_offset;
                // BUG FIX: Update ALSO the codec in TOC for files in the async chunk
                // Previously the codec was not updated, causing inconsistency
                final_toc[j].meta.codec = static_cast<uint8_t>(cr.codec);
            }
        }
        
        return true;
    };

    auto start_time = TarcUtil::safe_now();
    
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
        
        if (fsize > static_cast<uintmax_t>(SIZE_MAX)) {
            report_warning("File too large (overflow): " + disk_path);
            continue;
        }
        
        // ============================================================
        // Anti-OOM: large files → streaming (not loaded into RAM)
        // ============================================================
        bool use_streaming = (fsize > MAX_IN_MEMORY);
        
        std::vector<char> data;
        if (!use_streaming) {
            try {
                data.resize(static_cast<size_t>(fsize));
            } catch (const std::bad_alloc&) {
                use_streaming = true; // fallback a streaming se OOM
            }
        }

        bool read_ok = false;
        uint64_t h64 = 0;
        XXH64_state_t* const state = XXH64_createState();
        if (state) XXH64_reset(state, 0);
        
        FILE* in_f = fopen(disk_path.c_str(), "rb");
        if (in_f) {
            if (use_streaming) {
                // Streaming: calculate hash by reading in blocks (without loading all)
                std::vector<char> hbuf(64 * 1024);
                size_t n;
                while ((n = fread(hbuf.data(), 1, hbuf.size(), in_f)) > 0) {
                    if (state) XXH64_update(state, hbuf.data(), n);
                }
                read_ok = true;
            } else {
                size_t read_res = fread(data.data(), 1, static_cast<size_t>(fsize), in_f);
                if (read_res == static_cast<size_t>(fsize)) {
                    read_ok = true;
                    if (state) XXH64_update(state, data.data(), static_cast<size_t>(fsize));
                }
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

        // FEATURE #1 / #6: TOC codec reflects the codec ACTUALLY used
        Codec selected_codec;
    if (has_codec_override) {
        selected_codec = opts.codec;  // CLI override: --zstd, --lz4, etc.
    } else {
        selected_codec = CodecSelector::select(disk_path, fsize);  // auto-detect
    }

        // Timestamp: handle exceptions for in-use or special files (Windows)
        try {
            fe.meta.timestamp = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    fs::last_write_time(disk_path).time_since_epoch()
                ).count()
            );
        } catch (...) {
            fe.meta.timestamp = 0;  // fallback: no timestamp
        }

        if (hash_map.count(h64)) {
            fe.meta.is_duplicate = 1;
            fe.meta.duplicate_of_idx = hash_map[h64];
            fe.meta.codec = static_cast<uint8_t>(Codec::STORE);
            fe.meta.offset = 0; // duplicates have no data
            g_stats.duplicates_skipped++;
        } else {
            hash_map[h64] = static_cast<uint32_t>(final_toc.size());
            fe.meta.is_duplicate = 0;

            // ============================================================
            // STREAMING: large files compressed directly from disk
            // Does not load the file into RAM, uses ~128-256MB constant
            // ============================================================
            if (use_streaming && selected_codec != Codec::STORE) {
                // Flush pending solid buffer before streaming
                if (worker_active && !write_pending_chunk(future_chunk)) {
                    res.error = TarcError::CompressionFailed;
                    res.message = "Chunk compression failed.";
                    return res;
                }
                worker_active = false;

                if (!flush_solid_buffer(f, solid_buf, solid_has_files,
                                        solid_toc_begin, solid_codec, level,
                                        final_toc, res.bytes_out)) {
                    res.error = TarcError::CompressionFailed;
                    res.message = "Solid flush failed.";
                    return res;
                }

                // Stream-compress the large file directly
                uint64_t stream_offset;
                if (!tell_pos(f, stream_offset)) { res.error = TarcError::CorruptedArchive; res.message = "Failed to get file position."; return res; }
                Codec stream_codec = selected_codec;

                if (!write_chunk_streaming(f, disk_path, fsize, level, stream_codec, res.bytes_out)) {
                    res.error = TarcError::CompressionFailed;
                    res.message = "Streaming compression failed: " + disk_path;
                    return res;
                }

                // Update TOC
                fe.meta.offset = stream_offset;
                if (stream_codec == Codec::LZ4) stream_codec = Codec::ZSTD; // fallback
                fe.meta.codec = static_cast<uint8_t>(stream_codec);
                g_stats.bytes_read += fsize;
                int64_t _data_tell = IO::tarc_ftell(f);
                if (_data_tell < 0) { res.error = TarcError::CorruptedArchive; res.message = "Failed to get file position."; return res; }
                data_offset = static_cast<uint64_t>(_data_tell);
            } else if (use_streaming && selected_codec == Codec::STORE) {
                // ============================================================
                // BUG FIX #15: Streaming STORE — large non-compressible files
                // (.mp4, .avi, .jpg, etc.) that exceed MAX_IN_MEMORY.
                // Before: data was empty (0 bytes), the file was tracked in the
                // solid buffer without data → hash mismatch during verification.
                // Now: the file is copied directly from disk to archive
                // with a 1MB buffer, without loading into RAM.
                // ============================================================
                // Flush pending solid buffer
                if (worker_active && !write_pending_chunk(future_chunk)) {
                    res.error = TarcError::CompressionFailed;
                    res.message = "Chunk compression failed.";
                    return res;
                }
                worker_active = false;

                if (!flush_solid_buffer(f, solid_buf, solid_has_files,
                                        solid_toc_begin, solid_codec, level,
                                        final_toc, res.bytes_out)) {
                    res.error = TarcError::CompressionFailed;
                    res.message = "Solid flush failed.";
                    return res;
                }

                // Write the file as a direct STORE chunk (streaming)
                uint64_t store_offset;
                if (!tell_pos(f, store_offset)) { res.error = TarcError::CorruptedArchive; res.message = "Failed to get file position."; return res; }

                if (!write_chunk_store_streaming(f, disk_path, fsize, res.bytes_out)) {
                    res.error = TarcError::CompressionFailed;
                    res.message = "Streaming STORE failed: " + disk_path;
                    return res;
                }

                // Aggiorna TOC
                fe.meta.offset = store_offset;
                fe.meta.codec = static_cast<uint8_t>(Codec::STORE);
                g_stats.bytes_read += fsize;
                int64_t _data_tell = IO::tarc_ftell(f);
                if (_data_tell < 0) { res.error = TarcError::CorruptedArchive; res.message = "Failed to get file position."; return res; }
                data_offset = static_cast<uint64_t>(_data_tell);
            } else if (fsize <= STORE_THRESHOLD) {
                // ============================================================
                // BUG FIX: flush pending solid buffer BEFORE writing STORE chunk
                // Without this, solid chunks are written AFTER STORE chunks,
                // causing wrong ordering on disk and extraction failure.
                // ============================================================
                if (solid_has_files && !solid_buf.empty()) {
                    // Wait for any pending async compression
                    if (worker_active && !write_pending_chunk(future_chunk)) {
                        res.error = TarcError::CompressionFailed;
                        res.message = "Chunk compression failed.";
                        return res;
                    }
                    worker_active = false;

                    // Compress and write the accumulated solid buffer
                    ChunkResult solid_cr = compress_worker(std::move(solid_buf), level, solid_codec);
                    if (!solid_cr.success) {
                        res.error = TarcError::CompressionFailed;
                        res.message = "Solid chunk compression failed.";
                        return res;
                    }

                    uint64_t solid_offset;
                    if (!tell_pos(f, solid_offset)) { res.error = TarcError::CorruptedArchive; res.message = "Failed to get file position."; return res; }
                    if (!write_chunk(f, solid_cr.codec, solid_cr.raw_size, solid_cr.compressed_data, res.bytes_out)) {
                        res.error = TarcError::WriteFailed;
                        res.message = "Failed to write solid chunk.";
                        return res;
                    }

                    // Update Entry.offset for all files in this solid group
                    for (size_t j = solid_toc_begin; j < final_toc.size(); ++j) {
                        final_toc[j].meta.offset = solid_offset;
                    }
                    // Update TOC codec to the ACTUAL codec used
                    // (compress_worker may change codec to STORE for buffers < 4096)
                    Codec actual_codec = solid_cr.codec;
                    for (size_t j = solid_toc_begin; j < final_toc.size(); ++j) {
                        final_toc[j].meta.codec = static_cast<uint8_t>(actual_codec);
                    }

                    solid_buf.clear();
                    solid_buf.reserve(std::min(CHUNK_THRESHOLD, static_cast<size_t>(64 * 1024 * 1024)));
                    solid_has_files = false;
                    // Sync data_offset with the actual file position
                    int64_t _data_tell = IO::tarc_ftell(f);
                    if (_data_tell < 0) { res.error = TarcError::CorruptedArchive; res.message = "Failed to get file position."; return res; }
                    data_offset = static_cast<uint64_t>(_data_tell);
                }

                // BUG FIX #2: Empty files do not produce chunks on disk.
                // A chunk with raw_size=0 is confused with the end marker {0,0,0,0}
                // by the decompressor (read_next_block), causing "Archive corrupted".
                if (fsize == 0) {
                    // Empty file: add to TOC but do not write any chunk.
                    // The extractor skips files with orig_size==0.
                    fe.meta.offset = 0;
                    fe.meta.codec = static_cast<uint8_t>(Codec::STORE);
                    g_stats.bytes_read += 0;
                    // Do not increment data_offset because no chunk was written.
                    // BUG FIX #9: Do NOT reset solid_has_files here!
                    // The solid buffer was already flushed above.
                    // Resetting it here does nothing, but we remove the risk
                    // if the logic should change in the future.
                } else {
                    // STORE files: written directly, NOT in solid_buf
                    ChunkResult cr;
                    cr.compressed_data = data;
                    cr.raw_size = static_cast<uint32_t>(fsize);
                    cr.codec = Codec::STORE;
                    cr.success = true;

                    // FEATURE #5: offset of the STORE chunk
                    fe.meta.offset = data_offset;
                    fe.meta.codec = static_cast<uint8_t>(Codec::STORE);

                    if (!write_chunk(f, cr.codec, cr.raw_size, cr.compressed_data, res.bytes_out)) {
                        res.error = TarcError::WriteFailed;
                        res.message = "Failed to write STORE chunk.";
                        return res;
                    }
                    data_offset += sizeof(ChunkHeader) + data.size();
                    g_stats.bytes_read += fsize;
                    solid_has_files = false; // reset solid tracking
                }
            } else if (solid_buf.size() + fsize > CHUNK_THRESHOLD && !solid_buf.empty()) {
                // Solid buffer full: flush the current block
                
                // Write the previous pending chunk (if async compress in progress)
                if (worker_active && !write_pending_chunk(future_chunk)) {
                    res.error = TarcError::CompressionFailed;
                    res.message = "Chunk compression failed.";
                    return res;
                }
                worker_active = false;
                
                // Record the toc indices for this solid chunk about to be compressed
                pending_solid_ranges.push_back({solid_toc_begin, final_toc.size() - 1});
                
                // Start async compression for the current solid buffer
                // Respects the thread limit (--threads N)
                while (g_active_workers >= g_max_workers) {
                    std::this_thread::yield();
                }
                g_active_workers++;
                try {
                    future_chunk = std::async(
                        std::launch::async,
                        compress_worker,
                        std::move(solid_buf),
                        level,
                        solid_codec  // FEATURE #1: use the solid buffer codec
                    );
                } catch (const std::system_error&) {
                    // Thread creation failed: sync fallback
                    pending_solid_ranges.pop_back();
                    ChunkResult cr = compress_worker(std::move(solid_buf), level, solid_codec);
                    if (!cr.success) {
                        res.error = TarcError::CompressionFailed;
                        res.message = "Solid chunk compression failed (async fallback).";
                        return res;
                    }
                    uint64_t chunk_off;
                    if (!tell_pos(f, chunk_off)) { res.error = TarcError::CorruptedArchive; res.message = "Failed to get file position."; return res; }
                    if (!write_chunk(f, cr.codec, cr.raw_size, cr.compressed_data, res.bytes_out)) {
                        res.error = TarcError::WriteFailed;
                        res.message = "Failed to write solid chunk (async fallback).";
                        return res;
                    }
                    for (size_t j = solid_toc_begin; j < final_toc.size(); ++j) {
                        final_toc[j].meta.offset = chunk_off;
                        final_toc[j].meta.codec = static_cast<uint8_t>(cr.codec);
                    }
                    worker_active = false;
                    solid_buf.clear();
                    solid_buf.reserve(std::min(CHUNK_THRESHOLD, static_cast<size_t>(64 * 1024 * 1024)));
                    solid_toc_begin = final_toc.size();
                    solid_codec = selected_codec;
                    solid_has_files = true;
                    g_stats.bytes_read += fsize;
                    continue;
                }
                worker_active = true;
                solid_buf.clear();
                solid_buf.reserve(std::min(CHUNK_THRESHOLD, static_cast<size_t>(64 * 1024 * 1024)));

                // Start new solid generation with the current file
                solid_buf.insert(solid_buf.end(), data.begin(), data.end());
                solid_toc_begin = final_toc.size(); // questo file sara' a questo index
                solid_codec = selected_codec;       // codec of the new solid chunk
                solid_has_files = true;
                g_stats.bytes_read += fsize;
            } else try {
                // File goes into the current solid buffer
                solid_buf.insert(solid_buf.end(), data.begin(), data.end());
                g_stats.bytes_read += fsize;

                if (!solid_has_files) {
                    // First file of the solid chunk: record start and codec
                    solid_toc_begin = final_toc.size();
                    solid_codec = selected_codec;
                    solid_has_files = true;
                }
            } catch (const std::bad_alloc&) {
                // Solid buffer OOM: immediate flush and retry
                report_warning("[MEMORY] Solid buffer OOM, flushing early");
                if (worker_active && !write_pending_chunk(future_chunk)) {
                    res.error = TarcError::CompressionFailed;
                    res.message = "Chunk compression failed (OOM).";
                    return res;
                }
                worker_active = false;

                if (!flush_solid_buffer(f, solid_buf, solid_has_files,
                                        solid_toc_begin, solid_codec, level,
                                        final_toc, res.bytes_out)) {
                    res.error = TarcError::CompressionFailed;
                    res.message = "Solid flush failed (OOM).";
                    return res;
                }

                // BUG FIX #15b: If data is empty (file too large for RAM),
                // do not insert into solid buffer. Write as streaming STORE chunk.
                if (data.empty()) {
                    report_warning("[MEMORY] File too large for RAM, using streaming STORE: " + disk_path);
                    uint64_t store_offset;
                    if (!tell_pos(f, store_offset)) { res.error = TarcError::CorruptedArchive; res.message = "Failed to get file position."; return res; }
                    if (!write_chunk_store_streaming(f, disk_path, fsize, res.bytes_out)) {
                        res.error = TarcError::CompressionFailed;
                        res.message = "Streaming STORE failed (OOM): " + disk_path;
                        return res;
                    }
                    fe.meta.offset = store_offset;
                    fe.meta.codec = static_cast<uint8_t>(Codec::STORE);
                    g_stats.bytes_read += fsize;
                    int64_t _data_tell = IO::tarc_ftell(f);
                    if (_data_tell < 0) { res.error = TarcError::CorruptedArchive; res.message = "Failed to get file position."; return res; }
                    data_offset = static_cast<uint64_t>(_data_tell);
                    solid_has_files = false;
                } else {
                    // Retry after flush: single file as mini-chunk
                    try {
                        solid_buf = data; // copy singola
                        solid_toc_begin = final_toc.size();
                        solid_codec = selected_codec;
                        solid_has_files = true;
                        g_stats.bytes_read += fsize;
                    } catch (...) {
                        // The file itself is too large for RAM: fallback to streaming STORE
                        report_warning("[MEMORY] File too large for RAM, using streaming STORE: " + disk_path);
                        uint64_t store_offset;
                        if (!tell_pos(f, store_offset)) { res.error = TarcError::CorruptedArchive; res.message = "Failed to get file position."; return res; }
                        if (!write_chunk_store_streaming(f, disk_path, fsize, res.bytes_out)) {
                            res.error = TarcError::CompressionFailed;
                            res.message = "Streaming STORE failed: " + disk_path;
                            return res;
                        }
                        fe.meta.offset = store_offset;
                        fe.meta.codec = static_cast<uint8_t>(Codec::STORE);
                        g_stats.bytes_read += fsize;
                        int64_t _data_tell = IO::tarc_ftell(f);
                        if (_data_tell < 0) { res.error = TarcError::CorruptedArchive; res.message = "Failed to get file position."; return res; }
                        data_offset = static_cast<uint64_t>(_data_tell);
                        solid_has_files = false;
                    }
                }
            }
            
            // FEATURE #1: set TOC codec to the codec actually used
            // BUG FIX #15c: Do NOT overwrite the codec if the file was handled
            // by the streaming paths (use_streaming), which already set
            // the correct codec in the if/else block above.
            if (!use_streaming && fsize > STORE_THRESHOLD) {
                fe.meta.codec = static_cast<uint8_t>(solid_codec);
            }
        }
        
        UI::print_add(fe.name, fe.meta.orig_size,
                       static_cast<Codec>(fe.meta.codec),
                       fe.meta.is_duplicate ? 1.0f : 0.0f);
        final_toc.push_back(fe);
        g_stats.files_processed++;
    }

    // Write the last pending chunk (if async compression in progress)
    if (worker_active && !write_pending_chunk(future_chunk)) {
        res.error = TarcError::CompressionFailed;
        res.message = "Final chunk failed.";
        return res;
    }
    worker_active = false;
    
    // Write the final solid buffer (if not empty)
    if (!solid_buf.empty()) {
        ChunkResult last = compress_worker(std::move(solid_buf), level, solid_codec);
        
        // FEATURE #5: offset of the final chunk
        uint64_t last_chunk_offset;
        if (!tell_pos(f, last_chunk_offset)) { res.error = TarcError::CorruptedArchive; res.message = "Failed to get file position."; return res; }
        
        if (!write_chunk(f, last.codec, last.raw_size, last.compressed_data, res.bytes_out)) {
            res.error = TarcError::WriteFailed;
            res.message = "Failed to write final chunk.";
            return res;
        }
        
        // BUG FIX #4: Update ALSO the codec in TOC for files in the last solid chunk.
        // Previously only offset was updated but not the codec, causing inconsistency
        // when compress_worker changed codec (e.g., solid < 4096 bytes → STORE).
        Codec actual_last_codec = last.codec;
        for (size_t j = solid_toc_begin; j < final_toc.size(); ++j) {
            final_toc[j].meta.offset = last_chunk_offset;
            final_toc[j].meta.codec = static_cast<uint8_t>(actual_last_codec);
        }
    }

    if (fwrite(&END_MARKER, sizeof(END_MARKER), 1, f) != 1) {
        res.error = TarcError::WriteFailed;
        res.message = "Failed to write end marker.";
        return res;
    }
    IO::write_toc(f, h, final_toc);
    fflush(f);
    
    g_stats.bytes_in = g_stats.bytes_read;
    g_stats.bytes_out = res.bytes_out;
    g_stats.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        TarcUtil::safe_now() - start_time
    );
    
    res.ok = true;
    res.bytes_in = g_stats.bytes_read;
    res.bytes_out = g_stats.bytes_out;
    res.message = "Compression completed.";
    return res;
}

// ============================================================================
// Pattern matching
// ============================================================================

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

// ============================================================================
// ARCH-001: Helper to read the next decompressed chunk
// ============================================================================

static TarcError read_next_block(FILE* f, std::vector<char>& block, size_t& block_pos) {
    if (block_pos >= block.size()) {
        ChunkHeader ch;
        if (fread(&ch, sizeof(ch), 1, f) != 1) {
            return TarcError::CorruptedArchive;
        }

        // End marker detection: {0,0,0,0} — no legitimate chunk can
        // have all 4 fields zero (Codec::ZSTD=0 but comp_size > 0).
        if (std::memcmp(&ch, &END_MARKER, sizeof(ChunkHeader)) == 0) {
            return TarcError::CorruptedArchive;  // end marker raggiunto
        }

        if (ch.comp_size > TARC_MAX_CHUNK_SIZE || ch.raw_size > TARC_MAX_CHUNK_SIZE) {
            // Chunk size validation
            return TarcError::CorruptedArchive;
        }

        std::vector<char> comp(ch.comp_size);
        if (ch.comp_size > 0 && fread(comp.data(), 1, ch.comp_size, f) != ch.comp_size) {
            return TarcError::CorruptedArchive;
        }

        // BUG FIX #3: Verify chunk checksum (xxHash64 of compressed data)
        if (ch.comp_size > 0 && ch.checksum != 0) {
            uint64_t actual_cksum = XXH64(comp.data(), comp.size(), 0);
            if (actual_cksum != ch.checksum) {
                return TarcError::CorruptedArchive;
            }
        }

        // BUG FIX #12: raw_size must be reasonable
        if (ch.raw_size > TARC_MAX_CHUNK_SIZE) {
            return TarcError::CorruptedArchive;
        }

        block.resize(ch.raw_size);
        Codec codec = static_cast<Codec>(ch.codec);
        if (!decompress_chunk(comp, block, codec)) {
            return TarcError::DecompressionFailed;
        }
        // BUG FIX #13: Verify decompression produces the expected size.
        // If the decompressor produces a different size than ch.raw_size,
        // subsequent files in the solid block will have wrong offset.
        if (block.size() != static_cast<size_t>(ch.raw_size)) {
            return TarcError::DecompressionFailed;
        }
        block_pos = 0;
    }
    return TarcError::None;
}

// ============================================================================
// EXTRACT — Feature #4: output_dir support + ExtractOptions
// ============================================================================

TarcResult extract(const std::string& arch_path, const std::vector<std::string>& patterns,
                   ExtractOptions opts) {
    TarcResult res;
    res.ok = false;
    reset_stats();

    FileGuard fg(fopen(arch_path.c_str(), "rb")); FILE* f = fg.get();
    if (!fg.get()) {
        res.error = TarcError::FileNotFound;
        res.message = "Archive not found.";
        return res;
    }

    Header h;
    if (fread(&h, sizeof(h), 1, f) != 1) {
        res.error = TarcError::InvalidHeader;
        res.message = "Invalid header.";
        return res;
    }

    if (!IO::validate_archive_header(h)) {
        if (std::memcmp(h.magic, TARC_MAGIC, 4) != 0) {
            res.error = TarcError::InvalidHeader;
            res.message = "Not a valid TARC archive (bad magic).";
        } else {
            res.error = TarcError::UnsupportedVersion;
            res.message = "Unsupported archive version: " + std::to_string(h.version);
        }
        return res;
    }

    std::vector<FileEntry> toc;
    if (!IO::read_toc(f, h, toc)) {
        res.error = TarcError::CorruptedArchive;
        res.message = "Cannot read TOC.";
        return res;
    }
    
    // Seek to start of chunk data (right after header)
    if (IO::tarc_fseek(f, static_cast<int64_t>(sizeof(Header)), SEEK_SET) != 0) {
        res.error = TarcError::CorruptedArchive;
        res.message = "Failed to seek to chunk data.";
        return res;
    }
    
    std::vector<char> current_block;
    size_t block_pos = 0;
    std::map<std::string, int> flat_names_counter;

    // FEATURE #4: create output directory if specified
    if (!opts.output_dir.empty()) {
        fs::path out_dir(opts.output_dir);
        if (out_dir.is_relative()) {
            out_dir = fs::current_path() / out_dir;
        }
        std::error_code ec;
        fs::create_directories(out_dir, ec);
        if (ec) {
            res.error = TarcError::AccessDenied;
            res.message = "Cannot create output directory: " + opts.output_dir;
            return res;
        }
    }

    // Count real files (not duplicates, not empty) that must have chunk data
    size_t expected_real_files = 0;
    for (const auto& fe : toc) {
        if (!fe.meta.is_duplicate && fe.meta.orig_size > 0) expected_real_files++;
    }
    bool end_marker_hit = false;

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
            // BUG FIX #2: Empty files (orig_size==0) have no chunks on disk
            if (fe.meta.orig_size == 0) continue;
            // Skip ALL chunks for this file (potentially multi-chunk if >4GB)
            size_t remaining_skip = static_cast<size_t>(fe.meta.orig_size);
            while (remaining_skip > 0) {
                TarcError err = read_next_block(f, current_block, block_pos);
                if (err == TarcError::CorruptedArchive) {
                    end_marker_hit = true;
                    break;
                }
                if (err != TarcError::None) {
                    res.error = err;
                    res.message = "Chunk read failed.";
                    return res;
                }
                size_t available = current_block.size() - block_pos;
                size_t to_skip = std::min(remaining_skip, available);
                remaining_skip -= to_skip;
                block_pos += to_skip;
            }
            if (end_marker_hit) break;
            continue;
        }

        if (fe.meta.is_duplicate) continue;

        // BUG FIX #2: Empty files have no chunks on disk, skip reading
        if (fe.meta.orig_size == 0) {
            if (!opts.test_only) {
                std::string safe_path = IO::sanitize_extract_path(fe.name);
                if (!safe_path.empty()) {
                    std::string full_path = safe_path;
                    if (!opts.output_dir.empty()) {
                        std::string dir = opts.output_dir;
                        std::replace(dir.begin(), dir.end(), '\\', '/');
                        if (!dir.empty() && dir.back() != '/') dir += '/';
                        full_path = dir + safe_path;
                    }
                    fs::path p(full_path);
                    if (p.has_parent_path()) {
                        std::error_code ec;
                        fs::create_directories(p.parent_path(), ec);
                    }
                    std::ofstream out(full_path, std::ios::binary);
                }
            }
            UI::print_extract(fe.name, fe.meta.orig_size, opts.test_only, true);
            g_stats.files_processed++;
            continue;
        }

        std::string final_path = fe.name;
        if (opts.flat_mode) {
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

        // Potentially multi-chunk file reading
        size_t bytes_remaining = static_cast<size_t>(fe.meta.orig_size);

        XXH64_state_t* vstate = nullptr;
        if (opts.verify && fe.meta.xxhash != 0) {
            vstate = XXH64_createState();
            if (vstate) XXH64_reset(vstate, 0);
        }

        auto cleanup_vstate = [&]() { if (vstate) XXH64_freeState(vstate); };

        // Determine output path (only for test_only=false)
        std::string full_output_path;
        std::ofstream out_file;
        bool file_written = false;

        if (!opts.test_only) {
            std::string safe_path = IO::sanitize_extract_path(final_path);
            if (safe_path.empty()) {
                cleanup_vstate();
                report_warning("Path traversal blocked: " + fe.name);
                res.error = TarcError::PathTraversal;
                res.message = "Unsafe path in archive: " + fe.name;
                return res;
            }

            full_output_path = safe_path;
            if (!opts.output_dir.empty()) {
                std::string dir = opts.output_dir;
                std::replace(dir.begin(), dir.end(), '\\', '/');
                if (!dir.empty() && dir.back() != '/') {
                    dir += '/';
                }
                full_output_path = dir + safe_path;
            }
        }

        // Read chunks until we have the entire file
        while (bytes_remaining > 0) {
            TarcError err = read_next_block(f, current_block, block_pos);
            if (err == TarcError::CorruptedArchive) {
                cleanup_vstate();
                end_marker_hit = true;
                break;
            }
            if (err != TarcError::None) {
                cleanup_vstate();
                res.error = err;
                res.message = "Chunk read failed.";
                return res;
            }

            size_t available = current_block.size() - block_pos;
            size_t to_consume = std::min(bytes_remaining, available);

            if (!opts.test_only && to_consume > 0) {
                if (!file_written) {
                    fs::path p(full_output_path);
                    if (p.has_parent_path()) {
                        std::error_code ec;
                        fs::create_directories(p.parent_path(), ec);
                    }
                    out_file.open(full_output_path, std::ios::binary);
                    if (!out_file) {
                        cleanup_vstate();
                        res.error = TarcError::AccessDenied;
                        res.message = "Failed to write: " + full_output_path;
                        return res;
                    }
                    file_written = true;
                }
                out_file.write(current_block.data() + block_pos, static_cast<std::streamsize>(to_consume));
                if (!out_file) {
                    cleanup_vstate();
                    res.error = TarcError::WriteFailed;
                    res.message = "Write failed: " + full_output_path;
                    return res;
                }
            }

            if (vstate) {
                XXH64_update(vstate, current_block.data() + block_pos, to_consume);
            }

            bytes_remaining -= to_consume;
            block_pos += to_consume;
        }
        if (end_marker_hit) break;

        // Close file and set timestamp
        if (file_written) {
            out_file.close();
            if (fe.meta.timestamp != 0) {
                try {
                    auto ft = fs::file_time_type(std::chrono::seconds(
                        static_cast<time_t>(fe.meta.timestamp)));
                    fs::last_write_time(full_output_path, ft);
                } catch (...) {}
            }
        }

        // Verify xxHash integrity
        if (vstate) {
            uint64_t extracted_hash = XXH64_digest(vstate);
            cleanup_vstate();
            if (extracted_hash != fe.meta.xxhash) {
                res.error = TarcError::IntegrityCheckFailed;
                res.message = "Integrity check failed: " + fe.name;
                return res;
            }
        } else {
            cleanup_vstate();
        }

        UI::print_extract(fe.name, fe.meta.orig_size, opts.test_only, true);
        res.bytes_out += fe.meta.orig_size;
        g_stats.files_processed++;
    }
    if (end_marker_hit && g_stats.files_processed < expected_real_files) {
        res.ok = false;
        res.error = TarcError::CorruptedArchive;
        res.message = "Unexpected end of archive data (possible corruption).";
        return res;
    }
    res.ok = true;
    res.message = opts.test_only ? "Test completed." : "Extraction completed.";
    return res;
}

// ============================================================================
// LIST
// ============================================================================

TarcResult list(const std::string& arch_path, size_t offset) {
    TarcResult res;
    res.ok = false;
    
    FileGuard fg(fopen(arch_path.c_str(), "rb"));
    if (!fg.get()) {
        res.error = TarcError::FileNotFound;
        res.message = "Archive not found.";
        return res;
    }
    FILE* f = fg.get();
    
    if (offset > 0) {
        if (IO::tarc_fseek(f, static_cast<int64_t>(offset), SEEK_SET) != 0) {
            res.error = TarcError::CorruptedArchive;
            res.message = "Failed to seek in archive.";
            return res;
        }
    }
    
    Header h;
    if (fread(&h, sizeof(h), 1, f) != 1) {
        res.error = TarcError::InvalidHeader;
        res.message = "Invalid header.";
        return res;
    }

    if (!IO::validate_archive_header(h)) {
        if (std::memcmp(h.magic, TARC_MAGIC, 4) != 0) {
            res.error = TarcError::InvalidHeader;
            res.message = "Not a valid TARC archive (bad magic).";
        } else {
            res.error = TarcError::UnsupportedVersion;
            res.message = "Unsupported archive version: " + std::to_string(h.version);
        }
        return res;
    }

    std::vector<FileEntry> toc;
    h.toc_offset += offset;
    if (!IO::read_toc(f, h, toc)) {
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
