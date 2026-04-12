#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "types.h"

// ─── SELEZIONE AUTOMATICA CODEC ──────────────────────────────────────────────
namespace CodecSelector {
    // Seleziona automaticamente il codec migliore in base all'estensione e al livello.
    Codec select(const std::string& path, int level);

    // Stima se un buffer ha alta entropia
    bool is_high_entropy(const uint8_t* data, size_t len);
}

// ─── ENGINE (MODALITÀ SOLID & PARALLEL) ──────────────────────────────────────
namespace Engine {

    // [MOD v1.06] Struttura metadati con supporto Deduplicazione
    struct FileMeta {
        uint64_t orig_size;
        uint64_t timestamp;
        uint64_t xxhash;
        uint8_t  codec;
        bool     is_duplicate = false;      // Necessario per la deduplicazione
        uint32_t duplicate_of_idx = 0;      // Indice del file originale
    };

    struct FileEntry {
        std::string name;
        FileMeta meta;
    };

    // Strutture per il formato binario dell'archivio
    struct Header {
        char magic[4];
        uint16_t version;
    };

    struct ChunkHeader {
        uint32_t raw_size;
        uint32_t comp_size;
    };

    /**
     * Comprimi/aggiungi files nell'archivio.
     * [SOLID] I file vengono concatenati in blocchi grandi per massimizzare il ratio.
     * [DEDUPLICATION] Evita di comprimere dati identici calcolando l'hash XXH64.
     */
    TarcResult compress(
        const std::string&              arch_path,
        const std::vector<std::string>& files,
        bool                            append,
        int                             level
    );

    /**
     * Estrai o testa l'archivio.
     */
    TarcResult extract(
        const std::string& arch_path,
        bool               test_only
    );

    /**
     * Elimina file dall'archivio tramite pattern matching.
     */
    TarcResult remove_files(
        const std::string&              arch_path,
        const std::vector<std::string>& patterns
    );

    /**
     * Lista il contenuto dell'archivio.
     */
    TarcResult list(const std::string& arch_path);

} // namespace Engine
