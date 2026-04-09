#pragma once
#include <string>
#include <vector>
#include "types.h"

// ─── SELEZIONE AUTOMATICA CODEC ──────────────────────────────────────────────
namespace CodecSelector {

    // Estensioni che beneficiano di LZMA (ratio estremo)
    bool is_lzma_candidate(const std::string& filename);

    // Estensioni già compresse (LZ4 veloce o NONE)
    bool is_already_compressed(const std::string& filename);

    // Seleziona automaticamente il codec migliore per il file dato.
    // Logica:
    //   - File già compressi (.zip, .jpg, .mp4 ...): LZ4 (overhead minimo)
    //   - File testuali/codice (.txt, .cpp, .json ...): LZMA (ratio massimo)
    //   - Tutto il resto: ZSTD (bilanciato)
    Codec select_auto(const std::string& filename);

    // Stima velocemente se un buffer sembra già compresso
    // (analisi entropia sui primi N byte)
    bool is_high_entropy(const uint8_t* data, size_t len);

} // namespace CodecSelector

// ─── ENGINE ──────────────────────────────────────────────────────────────────
namespace Engine {

    // Comprimi/aggiungi files nell'archivio.
    // level: livello compressione (1-22 per ZSTD/LZMA, ignorato per LZ4)
    // append: true = modalità -a, false = crea nuovo archivio
    TarcResult compress(
        const std::string&              arch_path,
        const std::vector<std::string>& files,
        bool                            append,
        int                             level
    );

    // Estrai o testa l'archivio.
    // test_only: true = verifica hash senza scrivere su disco
    TarcResult extract(
        const std::string& arch_path,
        bool               test_only
    );

    // Elimina file dall'archivio (riscrive il file compattato).
    TarcResult remove_files(
        const std::string&              arch_path,
        const std::vector<std::string>& targets
    );

    // Lista il contenuto dell'archivio.
    TarcResult list(const std::string& arch_path);

} // namespace Engine
