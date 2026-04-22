#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace tarc {

// ═══════════════════════════════════════════════════════════════════════════
// CODEC ENUM
// ═══════════════════════════════════════════════════════════════════════════

enum class Codec : uint8_t {
    ZSTD  = 0,
    LZMA  = 1,
    STORE = 2,
    LZ4   = 3,
    BROTLI = 4
};

[[nodiscard]] inline const char* codec_name(Codec c) noexcept {
    switch (c) {
        case Codec::ZSTD:   return "ZSTD";
        case Codec::LZMA:   return "LZMA";
        case Codec::STORE:  return "STOR";
        case Codec::LZ4:    return "LZ4 ";
        case Codec::BROTLI: return "BROT";
        default:            return "????";
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// STRUTTURE DATI ARCHIVIO (Packed per compatibilità binaria)
// ═══════════════════════════════════════════════════════════════════════════

#pragma pack(push, 1)

struct Header {
    char     magic[4];      // "TRC2"
    uint32_t version;       // 200
    uint64_t toc_offset;    // Offset del TOC nell'archivio
    uint32_t file_count;    // Numero totale di file
};

struct Entry {
    uint64_t offset;              // Offset dati (relativo a inizio chunk stream)
    uint64_t orig_size;           // Dimensione originale
    uint64_t comp_size;           // Dimensione compressa (0 se duplicato)
    uint64_t xxhash;              // Hash XXH64 per deduplicazione
    uint64_t timestamp;           // Unix timestamp (secondi)
    uint32_t duplicate_of_idx;    // Indice file originale se duplicato
    uint16_t name_len;            // Lunghezza nome file
    uint8_t  codec;               // Codec utilizzato
    uint8_t  is_duplicate;        // 1 se duplicato, 0 altrimenti
};

struct ChunkHeader {
    uint32_t codec;      // Codec del chunk
    uint32_t raw_size;   // Dimensione dati non compressi
    uint32_t comp_size;  // Dimensione dati compressi
    uint64_t checksum;   // XXH64 dei dati decompressi
};

#pragma pack(pop)

// ═══════════════════════════════════════════════════════════════════════════
// FILE ENTRY (Entry + nome file)
// ═══════════════════════════════════════════════════════════════════════════

struct FileEntry {
    Entry       meta;
    std::string name;
    
    FileEntry() = default;
    FileEntry(const Entry& e, std::string n) 
        : meta(e), name(std::move(n)) {}
};

// ═══════════════════════════════════════════════════════════════════════════
// CHUNK RESULT (Risultato compressione chunk)
// ═══════════════════════════════════════════════════════════════════════════

struct ChunkResult {
    std::vector<char> data;
    uint32_t raw_size = 0;
    uint64_t checksum = 0;
    Codec codec = Codec::STORE;
    bool success = false;
};

} // namespace tarc
