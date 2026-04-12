#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "types.h" // Qui dentro ci sono già Header, TarcResult, ecc.

namespace CodecSelector {
    Codec select(const std::string& path, int level);
    bool is_high_entropy(const uint8_t* data, size_t len);
}

namespace Engine {

    // Usiamo le strutture definite in types.h per coerenza globale
    // Aggiungiamo solo i campi necessari alla deduplicazione se non ci sono in types.h
    // Se FileMeta è già in types.h, assicurati che abbia is_duplicate e duplicate_of_idx

    /**
     * Comprimi/aggiungi files nell'archivio.
     */
    TarcResult compress(
        const std::string&              arch_path,
        const std::vector<std::string>& files,
        bool                            append,
        int                             level
    );

    TarcResult extract(const std::string& arch_path, bool test_only);
    TarcResult list(const std::string& arch_path);
    TarcResult remove_files(const std::string& arch_path, const std::vector<std::string>& patterns);

} // namespace Engine
