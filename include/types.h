#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>

#define TARC_MAGIC     "TRC2"
#define TARC_VERSION   201  // Versione incrementata per nuove features
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
    uint32_t reserved[3]; // Per futuri usi
};

struct Entry {
    uint64_t offset;     
    uint64_t orig_size;  
    uint64_t comp_size;  
    uint64_t xxhash;     
    uint64_t timestamp;  
    uint32_t duplicate_of_idx; 
    uint32_t chunk_ref_idx;     // Nuovo: indice nel vettore chunk_refs
    uint32_t chunk_offset;      // Nuovo: offset nel chunk
    uint16_t name_len;   
    uint8_t  codec;      
    uint8_t  is_duplicate;     
    uint8_t  reserved[2];       // Allineamento
};

struct ChunkHeader {
    uint32_t codec;      
    uint32_t raw_size;   
    uint32_t comp_size;  
    uint64_t content_hash;  // XXH3 dell'header + dati compressi
};
#pragma pack(pop)

struct ChunkRef {
    uint64_t xxhash;
    uint32_t offset_in_block;
    uint32_t size;
    uint32_t ref_count;
};

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

// Hash map thread-safe per deduplicazione
class ConcurrentHashMap {
private:
    struct Entry {
        uint64_t hash;
        uint32_t index;
        std::string ext;  // Per skip list
    };
    std::vector<Entry> entries;
    mutable std::mutex mtx;
    
public:
    bool contains(uint64_t hash, const std::string& ext = "") const {
        std::lock_guard<std::mutex> lock(mtx);
        for (const auto& e : entries) {
            if (e.hash == hash) {
                // Skip .mdb se richiesto
                if (!ext.empty() && (ext == ".mdb" || ext == ".accdb" || ext == ".ldb")) {
                    return false;
                }
                return true;
            }
        }
        return false;
    }
    
    uint32_t get(uint64_t hash) const {
        std::lock_guard<std::mutex> lock(mtx);
        for (const auto& e : entries) {
            if (e.hash == hash) return e.index;
        }
        return 0;
    }
    
    void insert(uint64_t hash, uint32_t index, const std::string& ext = "") {
        std::lock_guard<std::mutex> lock(mtx);
        // Skip .mdb
        if (ext == ".mdb" || ext == ".accdb" || ext == ".ldb") return;
        entries.push_back({hash, index, ext});
    }
    
    void clear() {
        std::lock_guard<std::mutex> lock(mtx);
        entries.clear();
    }
};
