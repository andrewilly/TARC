#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "types.h"

namespace CodecSelector {
    // Seleziona il codec migliore in base all'estensione e alla dimensione
    Codec select(const std::string& path, size_t size);
    bool is_compressibile(const std::string& ext);
}

namespace Engine {

    // Funzioni Core Release 2.0
    TarcResult compress(const std::string& arch_path, const std::vector<std::string>& files, bool append, int level);
    
    // Nuova firma con flat_mode per estrazione senza percorsi
    TarcResult extract(const std::string& arch_path, const std::vector<std::string>& patterns = {}, bool test_only = false, size_t offset = 0, bool flat_mode = false);
    
    TarcResult list(const std::string& arch_path, size_t offset = 0);
    TarcResult remove_files(const std::string& arch_path, const std::vector<std::string>& patterns);
    
    // Specifica per generazione Autoestraente
    TarcResult create_sfx(const std::string& archive_path, const std::string& sfx_name);

} // namespace Engine
