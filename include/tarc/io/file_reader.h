#pragma once
#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include "tarc/util/result.h"

namespace tarc::io {

// ═══════════════════════════════════════════════════════════════════════════
// FILE READER INTERFACE (Abstract)
// ═══════════════════════════════════════════════════════════════════════════

class FileReader {
public:
    virtual ~FileReader() = default;
    
    /// Legge file completo in buffer
    [[nodiscard]] virtual Result read_file(
        const std::string& path, 
        std::vector<char>& buffer_out,
        uint64_t& hash_out
    ) = 0;
    
    /// Ottiene dimensione file
    [[nodiscard]] virtual Result get_file_size(
        const std::string& path,
        uint64_t& size_out
    ) = 0;
    
    /// Ottiene timestamp file (last write time)
    [[nodiscard]] virtual Result get_file_timestamp(
        const std::string& path,
        uint64_t& timestamp_out
    ) = 0;
    
    /// Verifica esistenza file
    [[nodiscard]] virtual bool file_exists(const std::string& path) const = 0;
};

// ═══════════════════════════════════════════════════════════════════════════
// FACTORY
// ═══════════════════════════════════════════════════════════════════════════

/// Crea FileReader appropriato per la piattaforma corrente
[[nodiscard]] std::unique_ptr<FileReader> create_file_reader();

// ═══════════════════════════════════════════════════════════════════════════
// FILE WRITER
// ═══════════════════════════════════════════════════════════════════════════

/// Scrive file su disco con timestamp e creazione cartelle
[[nodiscard]] Result write_file_to_disk(
    const std::string& path,
    const void* data,
    size_t size,
    uint64_t timestamp
);

} // namespace tarc::io
