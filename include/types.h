#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <chrono>
#include <functional>

#define TARC_MAGIC     "TRC2"
#define TARC_VERSION   200
#define TARC_VERSION_MIN 100
#define CHUNK_SIZE     (8 * 1024 * 1024)
#define TARC_EXT       ".strk"
#define TARC_MAX_CHUNK_SIZE  (2ULL * 1024 * 1024 * 1024)  // SEC-004: max 2GB per chunk (backward-compat)
#define TARC_MAX_FILE_SIZE   (8UL * 1024 * 1024 * 1024)  // SEC-004: max 8GB per singolo file
#define TARC_MAX_NAME_LEN    4096  // SEC-005: max filename length nel TOC (cross-platform)
#define SFX_MAGIC            "TARC_SFX"  // Magic per identificare archivi SFX autoestraenti
#define SFX_TRAILER_SIZE     24          // sizeof(SfxTrailer): 8 + 8 + 8 bytes

// ARCH-006: safe_now() centralizzato — evita duplicazione in engine.cpp, ui.cpp, main.cpp
#include <chrono>
namespace TarcUtil {
    inline std::chrono::steady_clock::time_point safe_now() {
        try {
            return std::chrono::steady_clock::now();
        } catch (...) {
            return std::chrono::steady_clock::time_point{};
        }
    }
}

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
    PathTraversal,
    IntegrityCheckFailed,
    UnsafeFilename,
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
        case TarcError::FileNotFound:       return "File not found";
        case TarcError::AccessDenied:      return "Access denied";
        case TarcError::CorruptedArchive: return "Archive is corrupted";
        case TarcError::InvalidHeader: return "Invalid archive header";
        case TarcError::InconsistentToc: return "TOC inconsistent with data";
        case TarcError::CompressionFailed: return "Compression failed";
        case TarcError::DecompressionFailed: return "Decompression failed";
        case TarcError::OutOfMemory:   return "Out of memory";
        case TarcError::UnsupportedVersion: return "Unsupported archive version";
        case TarcError::InvalidKey:    return "Invalid license key";
        case TarcError::LicenseMissing: return "License not found";
        case TarcError::DiskFull:     return "Disk full";
        case TarcError::Cancelled:    return "Operation cancelled";
        case TarcError::WriteFailed:    return "Write failed";
        case TarcError::PathTraversal: return "Path traversal detected";
        case TarcError::IntegrityCheckFailed: return "Integrity check failed";
        case TarcError::UnsafeFilename: return "Unsafe filename in archive";
        default:                     return "Unknown error";
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

// SFX Trailer — scritto alla FINE del file EXE autoestraente
// Layout: [Stub EXE][Archivio TARC .strk][SfxTrailer]
// Il stub legge gli ultimi 24 byte per trovare offset e dimensione dell'archivio.
struct SfxTrailer {
    char     magic[8];      // "TARC_SFX"
    uint64_t archive_offset; // offset dall'inizio del file dove inizia l'archivio TARC
    uint64_t archive_size;   // dimensione in byte dell'archivio TARC embeddato
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

    static TarcResult success() { return {}; }
    static TarcResult failure(TarcError e, const std::string& msg = {}) {
        return {false, e, msg, 0, 0, {}};
    }
};

struct CompressOptions {
    int level = 3;
    bool solid_mode = true;
    bool sfx_requested = false;
    bool verify = true;
    size_t chunk_size = 256 * 1024 * 1024;
    Codec codec = Codec::LZMA;  // FEATURE #6: codec override (LZMA = auto)
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
