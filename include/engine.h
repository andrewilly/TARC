#pragma once
#include <string>
#include <vector>
#include <string_view>
#include "types.h"

namespace CodecSelector {
    [[nodiscard]] Codec select(std::string_view path, size_t size) noexcept;
    [[nodiscard]] bool is_compressibile(std::string_view ext) noexcept;
    [[nodiscard]] bool should_skip_dedup(std::string_view path) noexcept;
    void set_skip_extensions(const std::vector<std::string>& exts);
}

namespace Engine {
    // Compressione
    [[nodiscard]] TarcResult compress(const std::string& arch_path, 
                                       const std::vector<std::string>& files, 
                                       bool append, 
                                       int level);
    
    // Estrazione con supporto wildcard e flat mode
    [[nodiscard]] TarcResult extract(const std::string& arch_path,
                                      const std::vector<std::string>& patterns = {},
                                      bool test_only = false, 
                                      size_t offset = 0, 
                                      bool flat_mode = false);
    
    // Lista contenuto
    [[nodiscard]] TarcResult list(const std::string& arch_path, size_t offset = 0);
    
    // Rimozione (non supportata in modalità solid)
    [[nodiscard]] TarcResult remove_files(const std::string& arch_path, 
                                           const std::vector<std::string>& patterns);
    
    // Generazione SFX
    [[nodiscard]] TarcResult create_sfx(const std::string& archive_path, 
                                         const std::string& sfx_name);
    
    // Configurazione runtime
    void set_chunk_threshold(size_t threshold);
    void set_compression_workers(size_t count);
}
