#pragma once
#include <cstdio>
#include <vector>
#include "tarc/util/types.h"
#include "tarc/util/result.h"

namespace tarc::io {

// ═══════════════════════════════════════════════════════════════════════════
// CHUNK STREAM MANAGER
// ═══════════════════════════════════════════════════════════════════════════

class ChunkStream {
public:
    explicit ChunkStream(FILE* file) : file_(file) {}
    
    /// Scrive chunk compresso con checksum
    [[nodiscard]] Result write_chunk(const ChunkResult& chunk);
    
    /// Scrive end marker (chunk con size 0)
    [[nodiscard]] Result write_end_marker();
    
    /// Legge prossimo chunk header
    [[nodiscard]] Result read_chunk_header(ChunkHeader& header_out);
    
    /// Legge chunk completo (header + dati compressi)
    [[nodiscard]] Result read_chunk(
        ChunkHeader& header_out,
        std::vector<char>& compressed_out
    );
    
    /// Decomprime chunk e verifica checksum
    [[nodiscard]] Result decompress_and_verify(
        const ChunkHeader& header,
        const std::vector<char>& compressed,
        std::vector<char>& decompressed_out
    );
    
private:
    FILE* file_;
};

} // namespace tarc::io
