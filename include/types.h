#pragma once
#include <cstdint>
#include <string>
#include <vector>

#define TARC_MAGIC     "STRK"
#define TARC_VERSION   110  // v1.10 Strike
#define CHUNK_SIZE     (4 * 1024 * 1024)
#define TARC_EXT       ".tar4"

enum class Codec : uint8_t {
    ZSTD = 0,
    LZ4  = 1,
    LZMA = 2,
    NONE = 3,
    BR   = 4  // Brotli
};

inline const char* codec_name(Codec c) {
    switch (c) {
        case Codec::ZSTD: return "ZSTD";
        case Codec::LZ4:  return "LZ4 ";
        case Codec::LZMA: return "7ZIP";
        case Codec::NONE: return "NONE";
        case Codec::BR:   return "BROT";
        default:          return "????";
    }
}

#pragma pack(push, 1)
struct Header {
    char     magic[4];   
    uint32_t version;    
    uint64_t toc_offset; 
    uint32_t file_count; // Numero totale di file nell'archivio
};

struct Entry {
    uint64_t offset;     
    uint64_t orig_size;  
    uint64_t comp_size;  
    uint64_t xxhash;     
    uint64_t timestamp;  
    uint16_t name_len;   
    uint8_t  codec;      
    uint8_t  _pad;       
};

struct ChunkHeader {
    uint32_t raw_size;   
    uint32_t comp_size;  
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
