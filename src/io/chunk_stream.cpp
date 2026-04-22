#include "tarc/io/chunk_stream.h"
#include "tarc/security/validator.h"
#include "tarc/security/checksum.h"
#include <cstring>
#include "lzma.h"

namespace tarc::io {

// ═══════════════════════════════════════════════════════════════════════════
// WRITE CHUNK
// ═══════════════════════════════════════════════════════════════════════════

Result ChunkStream::write_chunk(const ChunkResult& chunk) {
    ChunkHeader header{
        static_cast<uint32_t>(chunk.codec),
        chunk.raw_size,
        static_cast<uint32_t>(chunk.data.size()),
        chunk.checksum
    };
    
    // Scrivi header
    if (std::fwrite(&header, sizeof(header), 1, file_) != 1) {
        return Result::error(ErrorCode::CannotWriteFile, "Errore scrittura chunk header");
    }
    
    // Scrivi dati
    if (!chunk.data.empty()) {
        if (std::fwrite(chunk.data.data(), 1, chunk.data.size(), file_) != chunk.data.size()) {
            return Result::error(ErrorCode::DiskFull, "Errore scrittura chunk data");
        }
    }
    
    return Result::success();
}

// ═══════════════════════════════════════════════════════════════════════════
// WRITE END MARKER
// ═══════════════════════════════════════════════════════════════════════════

Result ChunkStream::write_end_marker() {
    ChunkHeader end_marker{0, 0, 0, 0};
    
    if (std::fwrite(&end_marker, sizeof(end_marker), 1, file_) != 1) {
        return Result::error(ErrorCode::CannotWriteFile, "Errore scrittura end marker");
    }
    
    return Result::success();
}

// ═══════════════════════════════════════════════════════════════════════════
// READ CHUNK HEADER
// ═══════════════════════════════════════════════════════════════════════════

Result ChunkStream::read_chunk_header(ChunkHeader& header_out) {
    if (std::fread(&header_out, sizeof(ChunkHeader), 1, file_) != 1) {
        if (std::feof(file_)) {
            return Result::error(ErrorCode::CorruptedChunk, "EOF prematuro");
        }
        return Result::error(ErrorCode::CannotReadFile, "Errore lettura chunk header");
    }
    
    // End marker detection
    if (header_out.raw_size == 0 && header_out.comp_size == 0) {
        return Result::success();  // End of stream
    }
    
    // Validazione dimensioni
    auto size_check = security::validate_chunk_size(header_out.raw_size);
    if (size_check.failed()) return size_check;
    
    size_check = security::validate_chunk_size(header_out.comp_size);
    if (size_check.failed()) return size_check;
    
    return Result::success();
}

// ═══════════════════════════════════════════════════════════════════════════
// READ CHUNK
// ═══════════════════════════════════════════════════════════════════════════

Result ChunkStream::read_chunk(
    ChunkHeader& header_out,
    std::vector<char>& compressed_out
) {
    auto header_result = read_chunk_header(header_out);
    if (header_result.failed()) return header_result;
    
    // End marker
    if (header_out.comp_size == 0) {
        compressed_out.clear();
        return Result::success();
    }
    
    // Leggi dati compressi
    try {
        compressed_out.resize(header_out.comp_size);
    } catch (const std::bad_alloc&) {
        return Result::error(ErrorCode::OutOfMemory,
            "Impossibile allocare buffer per chunk compresso");
    }
    
    if (std::fread(compressed_out.data(), 1, header_out.comp_size, file_) != header_out.comp_size) {
        return Result::error(ErrorCode::CorruptedChunk, "Lettura chunk incompleta");
    }
    
    return Result::success();
}

// ═══════════════════════════════════════════════════════════════════════════
// DECOMPRESS AND VERIFY
// ═══════════════════════════════════════════════════════════════════════════

Result ChunkStream::decompress_and_verify(
    const ChunkHeader& header,
    const std::vector<char>& compressed,
    std::vector<char>& decompressed_out
) {
    // Alloca buffer decompresso
    try {
        decompressed_out.resize(header.raw_size);
    } catch (const std::bad_alloc&) {
        return Result::error(ErrorCode::OutOfMemory,
            "Impossibile allocare buffer decompressione");
    }
    
    // Decompressione basata su codec
    Codec codec = static_cast<Codec>(header.codec);
    
    if (codec == Codec::STORE) {
        // Nessuna compressione - copia diretta
        if (compressed.size() != header.raw_size) {
            return Result::error(ErrorCode::CorruptedChunk,
                "Dimensione STORE non valida");
        }
        std::memcpy(decompressed_out.data(), compressed.data(), header.raw_size);
        
    } else if (codec == Codec::LZMA) {
        // Decompressione LZMA
        size_t src_pos = 0;
        size_t dst_pos = 0;
        uint64_t memlimit = UINT64_MAX;
        
        lzma_ret ret = lzma_stream_buffer_decode(
            &memlimit, 0, nullptr,
            reinterpret_cast<const uint8_t*>(compressed.data()),
            &src_pos, compressed.size(),
            reinterpret_cast<uint8_t*>(decompressed_out.data()),
            &dst_pos, header.raw_size
        );
        
        if (ret != LZMA_OK && ret != LZMA_STREAM_END) {
            return Result::error(ErrorCode::DecompressionFailed,
                "Errore decompressione LZMA (codice: " + std::to_string(ret) + ")");
        }
        
        if (dst_pos != header.raw_size) {
            return Result::error(ErrorCode::CorruptedChunk,
                "Dimensione decompresso non corrisponde");
        }
        
    } else {
        return Result::error(ErrorCode::CodecNotSupported,
            "Codec non implementato: " + std::to_string(header.codec));
    }
    
    // Verifica checksum
    if (header.checksum != 0) {
        auto checksum_result = security::verify_chunk_checksum(
            decompressed_out.data(),
            decompressed_out.size(),
            header.checksum
        );
        
        if (checksum_result.failed()) {
            return checksum_result;
        }
    }
    
    return Result::success();
}

} // namespace tarc::io
