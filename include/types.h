#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <chrono>
#include <functional>

#define TARC_MAGIC     "TRC2"
#define TARC_VERSION   200
#define CHUNK_SIZE     (8 * 1024 * 1024)
#define TARC_EXT       ".strk"

#ifdef _WIN32
    #define TARC_PATH_MAX 260
#else
    #define TARC_PATH_MAX 4096
#endif

enum class Codec : uint8_t {
    ZSTD = 0,
    LZMA = 1,
    STORE = 2,
    LZ4  = 3,
    BR   = 4,
    AES  = 5
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
    WrongPassword,
    EncryptionFailed,
    DecryptionFailed,
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
        case TarcError::WrongPassword: return "Wrong password";
        case TarcError::EncryptionFailed: return "Encryption failed";
        case TarcError::DecryptionFailed: return "Decryption failed";
        default:                     return "Unknown error";
    }
}

namespace Crypto {
    uint64_t derive_key(const std::string& password, uint64_t salt);
    
    std::vector<char> encrypt_chunk(const std::vector<char>& data, const std::string& password);
    bool decrypt_chunk(const std::vector<char>& encrypted, std::vector<char>& decrypted, const std::string& password);
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
    uint64_t checksum;   // XXH64 of raw data
    uint64_t reserved;   // For future use
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
    size_t chunk_size = 64 * 1024 * 1024;
    std::string password;
    bool encrypt = false;
};

struct ExtractOptions {
    bool test_only = false;
    bool flat_mode = false;
    bool verify = true;
    bool overwrite = false;
    std::string output_dir;
    std::string password;
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
