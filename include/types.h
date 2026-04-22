#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <map>

#define TARC_MAGIC     "TRC2"
#define TARC_VERSION   203
#define CHUNK_SIZE     (8 * 1024 * 1024)
#define TARC_EXT       ".strk"

// ─── COSTANTI NOMINATE ────────────────────────────────────────────────────────
// Soglia dimensione chunk solid (256 MB)
constexpr size_t CHUNK_THRESHOLD       = 256ULL * 1024 * 1024;
// Soglia per codec switch: sotto questa dimensione ZSTD, sopra LZMA (512 KB)
constexpr size_t CODEC_SWITCH_SIZE     = 512ULL * 1024;
// Dimensione massima nome file nel TOC
constexpr uint16_t MAX_NAME_LEN        = 4096;

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

// ─── INTERVENTO #18: TARCRISULT ARRICCHITO ─────────────────────────────────────
// Aggiunge statistiche dettagliate: conteggio file/duplicati, per-codec,
// tempo impiegato, dimensione archivio su disco.
struct TarcResult {
    bool        ok          = true;
    std::string message;

    // Statistiche base
    uint64_t    bytes_in    = 0;
    uint64_t    bytes_out   = 0;

    // Conteggi
    uint32_t    file_count  = 0;
    uint32_t    dup_count   = 0;
    uint32_t    skip_count  = 0;    // File saltati (exclude o errori)

    // Statistiche per-codec
    std::map<Codec, uint64_t> codec_bytes;     // Byte compressi per codec
    std::map<Codec, uint32_t> codec_chunks;    // Chunk per codec

    // Tempo impiegato (millisecondi)
    uint64_t    elapsed_ms  = 0;

    // Dimensione archivio su disco
    uint64_t    archive_size = 0;
};
