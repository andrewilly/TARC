#pragma once
#include <cstdint>
#include <string>
#include <vector>

#define TARC_MAGIC     "TRC2"
#define TARC_VERSION   201
#define CHUNK_SIZE     (8 * 1024 * 1024)
#define TARC_EXT       ".strk"

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
    uint64_t checksum;   // XXH64 del dato compresso (0 = non verificato, retrocompatibile)
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
