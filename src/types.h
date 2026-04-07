#pragma once
#include <cstdint>
#include <string>
#include <vector>

// ─── MAGIC & VERSION ─────────────────────────────────────────────────────────
#define TARC_MAGIC     "TARC"
#define TARC_VERSION   102
#define CHUNK_SIZE     (4 * 1024 * 1024)
#define TARC_EXT       ".tar4"

// ─── ALGORITMI DI COMPRESSIONE ───────────────────────────────────────────────
enum class Codec : uint8_t {
    ZSTD = 0,   // General purpose (default)
    LZ4  = 1,   // Alta velocità (file già compressi, dati binari)
    LZMA = 2,   // Ratio massimo (testo, codice sorgente)
    NONE = 3    // Nessuna compressione (file già compressi)
};

inline const char* codec_name(Codec c) {
    switch (c) {
        case Codec::ZSTD: return "ZSTD";
        case Codec::LZ4:  return "LZ4 ";
        case Codec::LZMA: return "LZMA";
        case Codec::NONE: return "NONE";
        default:          return "????";
    }
}

// ─── STRUTTURE BINARIE (packed) ───────────────────────────────────────────────
#pragma pack(push, 1)

struct Header {
    char     magic[4];   // "TARC"
    uint32_t version;    // versione formato
    uint64_t toc_offset; // offset della TOC nel file
};

struct Entry {
    uint64_t offset;     // offset dei dati compressi nell'archivio
    uint64_t orig_size;  // dimensione originale (decompressa)
    uint64_t comp_size;  // dimensione compressa (inclusi header chunk)
    uint64_t xxhash;     // hash XXH64 del dato originale
    uint16_t name_len;   // lunghezza del nome file
    uint8_t  codec;      // Codec usato (enum Codec)
    uint8_t  _pad;       // padding allineamento
};

struct ChunkHeader {
    uint32_t raw_size;   // dimensione originale del chunk
    uint32_t comp_size;  // dimensione compressa del chunk
};

#pragma pack(pop)

// ─── STRUTTURA LOGICA ─────────────────────────────────────────────────────────
struct FileEntry {
    Entry       meta;
    std::string name;
};

// ─── RISULTATO OPERAZIONI ─────────────────────────────────────────────────────
struct TarcResult {
    bool        ok      = true;
    std::string message;
    uint64_t    bytes_in  = 0;
    uint64_t    bytes_out = 0;
};
