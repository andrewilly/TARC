#pragma once
#include <string>
#include <vector>
#include "types.h"

// ─── SELEZIONE AUTOMATICA CODEC ──────────────────────────────────────────────
namespace CodecSelector {

    // Seleziona automaticamente il codec migliore in base all'estensione e al livello.
    // [MOD v1.05] Ora integra Brotli per i livelli "Ultra" e gestisce la logica Solid.
    Codec select(const std::string& path, int level);

    // Stima se un buffer ha alta entropia (usato per decidere se saltare la compressione)
    bool is_high_entropy(const uint8_t* data, size_t len);

} // namespace CodecSelector

// ─── ENGINE (MODALITÀ SOLID & PARALLEL) ──────────────────────────────────────
namespace Engine {

    /**
     * Comprimi/aggiungi files nell'archivio.
     * [SOLID] I file vengono concatenati in blocchi da 16MB per massimizzare il ratio.
     * [PARALLEL] I blocchi vengono compressi usando tutti i core disponibili.
     * * @param arch_path Percorso dell'archivio .tar4
     * @param files     Lista dei percorsi dei file da aggiungere
     * @param append    Se true, aggiunge alla fine (disabilita Solid per quel blocco)
     * @param level     Livello di compressione (1-22). 22 attiva Brotli/LZMA Extreme.
     */
    TarcResult compress(
        const std::string&              arch_path,
        const std::vector<std::string>& files,
        bool                            append,
        int                             level
    );

    /**
     * Estrai o testa l'archivio.
     * [SOLID] Legge l'archivio sequenzialmente per ricostruire i file dai blocchi.
     * * @param arch_path Percorso dell'archivio da leggere
     * @param test_only Se true, verifica solo l'integrità tramite XXH64
     */
    TarcResult extract(
        const std::string& arch_path,
        bool               test_only
    );

    /**
     * Elimina file dall'archivio tramite pattern matching.
     * Nota: In modalità Solid, questa operazione richiede la ricostruzione dei blocchi.
     */
    TarcResult remove_files(
        const std::string&              arch_path,
        const std::vector<std::string>& patterns
    );

    /**
     * Lista il contenuto dell'archivio mostrando ratio e codec usato per ogni file.
     */
    TarcResult list(const std::string& arch_path);

} // namespace Engine
