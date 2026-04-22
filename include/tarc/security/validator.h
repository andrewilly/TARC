#pragma once
#include <string>
#include <cstdint>
#include <filesystem>
#include "tarc/util/result.h"

namespace tarc::security {

// ═══════════════════════════════════════════════════════════════════════════
// PATH VALIDATION (Anti Path-Traversal)
// ═══════════════════════════════════════════════════════════════════════════

/// Verifica che il path non esca dalla directory di estrazione
[[nodiscard]] Result validate_extraction_path(
    const std::string& path, 
    const std::filesystem::path& base_dir = std::filesystem::current_path()
);

/// Normalizza path rimuovendo componenti pericolosi (.. / .)
[[nodiscard]] std::string sanitize_path(const std::string& path);

/// Verifica che il path sia relativo (no absolute paths)
[[nodiscard]] bool is_safe_relative_path(const std::string& path);

// ═══════════════════════════════════════════════════════════════════════════
// SIZE VALIDATION
// ═══════════════════════════════════════════════════════════════════════════

/// Valida dimensione chunk
[[nodiscard]] Result validate_chunk_size(size_t size);

/// Valida lunghezza nome file
[[nodiscard]] Result validate_filename_length(size_t len);

/// Valida numero totale di file
[[nodiscard]] Result validate_file_count(uint32_t count);

// ═══════════════════════════════════════════════════════════════════════════
// TIMESTAMP VALIDATION
// ═══════════════════════════════════════════════════════════════════════════

/// Valida e clamp timestamp a range sicuro
[[nodiscard]] uint64_t sanitize_timestamp(uint64_t ts);

/// Verifica timestamp nel range valido (1970-2100)
[[nodiscard]] bool is_valid_timestamp(uint64_t ts);

// ═══════════════════════════════════════════════════════════════════════════
// ARCHIVE VALIDATION
// ═══════════════════════════════════════════════════════════════════════════

/// Valida header archivio
[[nodiscard]] Result validate_header(const class Header& header, size_t file_size);

/// Valida offset TOC
[[nodiscard]] Result validate_toc_offset(uint64_t offset, size_t file_size);

} // namespace tarc::security
