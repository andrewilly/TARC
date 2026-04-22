#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <atomic>
#include <shared_mutex>
#include <optional>
#include <mutex>   // <--- AGGIUNGI QUESTA RIGA

#define TARC_MAGIC     "TRC2"
#define TARC_VERSION   200
#define CHUNK_SIZE     (8 * 1024 * 1024)
#define TARC_EXT       ".strk"

enum class Codec : uint8_t {
    ZSTD = 0,
    LZMA = 1,
    STORE = 2,
    LZ4  = 3,
    BR   = 4  
};

inline constexpr const char* codec_name(Codec c) noexcept {
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
    uint32_t reserved[3];
};

struct Entry {
    uint64_t offset;
    uint64_t orig_size;
    uint64_t comp_size;
    uint64_t xxhash;
    uint64_t timestamp;
    uint32_t duplicate_of_idx;
    uint32_t chunk_ref_idx;
    uint32_t chunk_offset;
    uint16_t name_len;
    uint8_t  codec;
    uint8_t  is_duplicate;
    uint8_t  reserved[2];
};

struct ChunkHeader {
    uint32_t codec;
    uint32_t raw_size;
    uint32_t comp_size;
    uint64_t content_hash;
};
#pragma pack(pop)

struct FileEntry {
    Entry       meta{};
    std::string name{};
};

struct TarcResult {
    bool        ok = true;
    std::string message;
    uint64_t    bytes_in = 0;
    uint64_t    bytes_out = 0;
};

// Thread-safe hash map per deduplicazione
class ConcurrentHashMap {
private:
    struct Entry {
        uint64_t hash;
        uint32_t index;
        std::string ext;
    };
    std::vector<Entry> entries;
    mutable std::shared_mutex mtx;
    
    static bool should_skip_ext(const std::string& ext) noexcept {
        return ext == ".mdb" || ext == ".accdb" || ext == ".ldb";
    }
    
public:
    bool contains(uint64_t hash, const std::string& ext = "") const {
        std::shared_lock lock(mtx);
        for (const auto& e : entries) {
            if (e.hash == hash) {
                if (!ext.empty() && should_skip_ext(ext)) return false;
                return true;
            }
        }
        return false;
    }
    
    std::optional<uint32_t> get(uint64_t hash) const {
        std::shared_lock lock(mtx);
        for (const auto& e : entries) {
            if (e.hash == hash) return e.index;
        }
        return std::nullopt;
    }
    
    void insert(uint64_t hash, uint32_t index, const std::string& ext = "") {
        if (should_skip_ext(ext)) return;
        std::unique_lock lock(mtx);
        entries.push_back({hash, index, ext});
    }
    
    void clear() {
        std::unique_lock lock(mtx);
        entries.clear();
    }
    
    size_t size() const {
        std::shared_lock lock(mtx);
        return entries.size();
    }
};
