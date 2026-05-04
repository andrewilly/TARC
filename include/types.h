#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <chrono>
#include <functional>
#include <cstdio>

#define TARC_MAGIC     "TRC2"
#define TARC_VERSION   200
#define CHUNK_SIZE     (8 * 1024 * 1024)
#define TARC_EXT       ".strk"

// Security constants (SEC-004, SEC-005)
// SEC-004 FIX: raised from 512MB to 2GB — old code used 1GB solid threshold,
// so archives created before v2.01 may contain chunks > 512MB.
// 2GB still prevents malicious OOM while being backward-compatible.
#define TARC_MAX_CHUNK_SIZE    (2ULL * 1024 * 1024 * 1024)  // 2 GB anti-OOM
#define TARC_MAX_NAME_LEN      4096                     // cross-platform
#define TARC_VERSION_MIN       100                      // minimum accepted version

enum class Codec : uint8_t {
    ZSTD = 0,
    LZMA = 1,
    STORE = 2,
    LZ4  = 3,
    BR   = 4
};

enum class TarcError : uint32_t {
    None = 0,
    FileNotFound,
    AccessDenied,
    CorruptedArchive,
    InvalidHeader,
    InconsistentToc,
    CompressionFailed,
    DecompressionFailed,
    OutOfMemory,
    UnsupportedVersion,
    InvalidKey,
    LicenseMissing,
    DiskFull,
    Cancelled,
    WriteFailed,
    // Security error codes
    InvalidMagic,           // SEC-001
    InvalidVersion,         // SEC-003
    IntegrityCheckFailed,   // SEC-006
    PathTraversalDetected,  // SEC-002
    Unknown
};

inline const char* codec_name(Codec c) {
    switch (c) {
        case Codec::ZSTD:  return "ZSTD";
        case Codec::LZMA:  return "LZMA";
        case Codec::STORE: return "STOR";
        case Codec::LZ4:   return "LZ4 ";
        case Codec::BR:    return "BROT";
        default:           return "????";
    }
}

inline const char* error_message(TarcError e) {
    switch (e) {
        case TarcError::None:                return "Success";
        case TarcError::FileNotFound:        return "File not found";
        case TarcError::AccessDenied:        return "Access denied";
        case TarcError::CorruptedArchive:    return "Archive is corrupted";
        case TarcError::InvalidHeader:       return "Invalid archive header";
        case TarcError::InconsistentToc:     return "TOC inconsistent with data";
        case TarcError::CompressionFailed:   return "Compression failed";
        case TarcError::DecompressionFailed: return "Decompression failed";
        case TarcError::OutOfMemory:         return "Out of memory";
        case TarcError::UnsupportedVersion:  return "Unsupported archive version";
        case TarcError::InvalidKey:          return "Invalid license key";
        case TarcError::LicenseMissing:      return "License not found";
        case TarcError::DiskFull:            return "Disk full";
        case TarcError::Cancelled:           return "Operation cancelled";
        case TarcError::WriteFailed:         return "Write failed";
        case TarcError::InvalidMagic:        return "Invalid archive magic";
        case TarcError::InvalidVersion:      return "Invalid archive version";
        case TarcError::IntegrityCheckFailed:return "Integrity check failed";
        case TarcError::PathTraversalDetected:return "Path traversal detected";
        default:                             return "Unknown error";
    }
}

#pragma pack(push, 1)
struct Header {
    char     magic[4];
    uint32_t version;
    uint64_t toc_offset;
    uint32_t file_count;
};

struct Entry {
    uint64_t offset;
    uint64_t orig_size;
    uint64_t comp_size;
    uint64_t xxhash;
    uint64_t timestamp;
    uint32_t duplicate_of_idx;
    uint16_t name_len;
    uint8_t  codec;
    uint8_t  is_duplicate;
};

struct ChunkHeader {
    uint32_t codec;
    uint32_t raw_size;
    uint32_t comp_size;
    uint64_t checksum;
};
#pragma pack(pop)

struct FileEntry {
    Entry       meta;
    std::string name;
};

struct TarcResult {
    bool        ok      = true;
    TarcError   error   = TarcError::None;
    std::string message;
    uint64_t    bytes_in  = 0;
    uint64_t    bytes_out = 0;
    std::vector<std::string> warnings;
    std::vector<FileEntry> entries;  // ARCH-012: populated by list() for UI decoupling

    static TarcResult success() { return {}; }
    static TarcResult failure(TarcError e, const std::string& msg = {}) {
        return {false, e, msg, 0, 0, {}, {}};
    }
};

struct CompressOptions {
    int level = 3;
    bool solid_mode = true;
    bool sfx_requested = false;
    bool verify = true;
    size_t chunk_size = 256 * 1024 * 1024;
    int threads = 0;  // 0 = auto-detect (use hardware_concurrency)
};

struct ExtractOptions {
    bool test_only = false;
    bool flat_mode = false;
    bool verify = true;
    bool overwrite = false;
    std::string output_dir;
};

template<typename T>
struct Result {
    TarcError err = TarcError::None;
    std::optional<T> value;

    bool has_value() const { return value.has_value(); }
    T& operator*() { return *value; }
    const T& operator*() const { return *value; }
    explicit operator bool() const { return value.has_value(); }
};

// Common utility: safe steady_clock access (centralized — ARCH-003)
inline std::chrono::steady_clock::time_point safe_now() {
    try {
        return std::chrono::steady_clock::now();
    } catch (...) {
        return std::chrono::steady_clock::time_point{};
    }
}

// ARCH-010: RAII wrapper for FILE* to prevent handle leaks
class FileGuard {
public:
    explicit FileGuard(FILE* f = nullptr) : f_(f) {}
    ~FileGuard() { close(); }

    FileGuard(const FileGuard&) = delete;
    FileGuard& operator=(const FileGuard&) = delete;
    FileGuard(FileGuard&& other) noexcept : f_(other.f_) { other.f_ = nullptr; }
    FileGuard& operator=(FileGuard&& other) noexcept {
        if (this != &other) { close(); f_ = other.f_; other.f_ = nullptr; }
        return *this;
    }

    FILE* get() const { return f_; }
    explicit operator bool() const { return f_ != nullptr; }
    FILE* release() { FILE* tmp = f_; f_ = nullptr; return tmp; }
    void close() { if (f_) { fclose(f_); f_ = nullptr; } }

private:
    FILE* f_;
};
