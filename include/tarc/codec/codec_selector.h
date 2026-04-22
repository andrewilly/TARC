#pragma once
#include <string>
#include "tarc/util/types.h"

namespace tarc::codec {

// ═══════════════════════════════════════════════════════════════════════════
// CODEC SELECTOR
// ═══════════════════════════════════════════════════════════════════════════

/// Seleziona codec ottimale basato su estensione e dimensione file
[[nodiscard]] Codec select_codec(const std::string& filename, size_t file_size);

/// Verifica se file è comprimibile (skip archivi già compressi)
[[nodiscard]] bool is_compressible(const std::string& extension);

/// Comprime buffer con codec specificato
[[nodiscard]] ChunkResult compress_buffer(
    const std::vector<char>& input,
    Codec codec,
    int compression_level
);

} // namespace tarc::codec
