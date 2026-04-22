#pragma once
#include <cstdint>
#include <string>
#include <vector>

#define TARC_MAGIC     "TRC2"
#define TARC_VERSION   200  
#define TARC_MIN_VERSION 200
#define CHUNK_SIZE     (8 * 1024 * 1024)
#define TARC_EXT       ".strk"

// Flag per header esteso
#define TARC_FLAG_HAS_CRC32    0x01
#define TARC_FLAG_HAS_SIGNATURE 0x02

enum class Codec : uint8_t {
    ZSTD = 0,
    LZMA = 1,
    STORE = 2,
    LZ4  = 3,
    BR   = 4  
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

#pragma pack(push, 1)

struct Header {
    char     magic[4];        // "TRC2"
    uint32_t version;         // 200
    uint64_t toc_offset;      // Offset del TOC
    uint32_t file_count;      // Numero di file
    uint32_t flags;           // Flags estesi
    uint32_t header_checksum; // CRC32 dell'header
    uint32_t reserved[2];     // Per futuri usi
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
    uint32_t chunk_crc32;      // CRC32 del chunk
};

struct ChunkHeader {
    uint32_t codec;      
    uint32_t raw_size;   
    uint32_t comp_size;  
    uint64_t checksum;   // XXH64 per integrità
    uint32_t crc32;      // CRC32 veloce per corruzione
    uint32_t reserved;
};
#pragma pack(pop)

struct FileEntry {
    Entry       meta;
    std::string name;
};

struct TarcResult {
    bool        ok      = true;
    std::string message;
    uint64_t    bytes_in  = 0;
    uint64_t    bytes_out = 0;
};
