#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <map>

// ─── COSTANTI FORMATO ARCHIVIO ──────────────────────────────────────────────
constexpr const char* TARC_MAGIC        = "TRC2";
constexpr uint32_t    TARC_VERSION      = 204;
constexpr size_t      CHUNK_SIZE        = 8ULL * 1024 * 1024;
constexpr const char* TARC_EXT          = ".strk";
constexpr const char* SFX_TRAILER_MAGIC = "TSFX";

// ─── COSTANTI DI SICUREZZA ──────────────────────────────────────────────────
// Soglia dimensione chunk solid (256 MB)
constexpr size_t CHUNK_THRESHOLD       = 256ULL * 1024 * 1024;
// Soglia per codec switch: sotto questa dimensione ZSTD, sopra LZMA (512 KB)
constexpr size_t CODEC_SWITCH_SIZE     = 512ULL * 1024;
// Dimensione massima nome file nel TOC
constexpr uint16_t MAX_NAME_LEN        = 4096;
// Numero massimo di file nell'archivio (protezione OOM da header malevolo)
constexpr uint32_t MAX_FILE_COUNT      = 1'000'000;
// Dimensione massima dati compressi in un singolo chunk (protezione OOM)
constexpr uint32_t MAX_CHUNK_COMP_SIZE = 512ULL * 1024 * 1024;

enum class Codec : uint8_t {
    ZSTD  = 0,
    LZMA  = 1,
    STORE = 2,
    LZ4   = 3,
    BR    = 4
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
    uint64_t offset           = 0;
    uint64_t orig_size        = 0;
    uint64_t comp_size        = 0;
    uint64_t xxhash           = 0;
    uint64_t timestamp        = 0;
    uint32_t duplicate_of_idx = 0;
    uint16_t name_len         = 0;
    uint8_t  codec            = 0;
    uint8_t  is_duplicate     = 0;
};

struct ChunkHeader {
    uint32_t codec    = 0;
    uint32_t raw_size = 0;
    uint32_t comp_size = 0;
    uint64_t checksum = 0;   // XXH64 del dato compresso (0 = non verificato, retrocompatibile)
};

// ─── SFX TRAILER ────────────────────────────────────────────────────────────
// Contiene l'offset esatto dove inizia l'archivio TRC2, cosi' lo stub
// non deve scansionare tutto il file per trovare il magic "TRC2".
struct SFXTrailer {
    uint64_t archive_offset = 0;   // Offset inizio archivio (dopo lo stub)
    char     magic[4];             // "TSFX" — firma del trailer
};
#pragma pack(pop)

struct FileEntry {
    Entry       meta;
    std::string name;
};

// ─── RISULTATO OPERAZIONE ───────────────────────────────────────────────────
struct TarcResult {
    bool        ok          = true;
    std::string message;

    // Statistiche base
    uint64_t    bytes_in    = 0;
    uint64_t    bytes_out   = 0;

    // Conteggi
    uint32_t    file_count  = 0;
    uint32_t    dup_count   = 0;
    uint32_t    skip_count  = 0;

    // Statistiche per-codec
    std::map<Codec, uint64_t> codec_bytes;
    std::map<Codec, uint32_t> codec_chunks;

    // Tempo impiegato (millisecondi)
    uint64_t    elapsed_ms  = 0;

    // Dimensione archivio su disco
    uint64_t    archive_size = 0;
};
