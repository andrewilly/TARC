#include "tarc/codec/codec_selector.h"
#include "tarc/security/checksum.h"
#include "tarc/util/constants.h"
#include <algorithm>
#include <set>
#include <filesystem>
#include "lzma.h"

namespace tarc::codec {

// ═══════════════════════════════════════════════════════════════════════════
// COMPRESSIBLE CHECK
// ═══════════════════════════════════════════════════════════════════════════

bool is_compressible(const std::string& extension) {
    static const std::set<std::string> skip_extensions = {
        ".zip", ".7z", ".rar", ".gz", ".bz2", ".xz", ".lzma",
        ".jpg", ".jpeg", ".png", ".mp4", ".mp3", ".avi", ".mkv"
    };
    
    std::string ext_lower = extension;
    std::transform(ext_lower.begin(), ext_lower.end(), ext_lower.begin(), ::tolower);
    
    return skip_extensions.find(ext_lower) == skip_extensions.end();
}

// ═══════════════════════════════════════════════════════════════════════════
// CODEC SELECTION
// ═══════════════════════════════════════════════════════════════════════════

Codec select_codec(const std::string& filename, size_t file_size) {
    std::filesystem::path p(filename);
    std::string ext = p.extension().string();
    
    // File non comprimibili → STORE
    if (!is_compressible(ext)) {
        return Codec::STORE;
    }
    
    // File piccoli → ZSTD (veloce)
    if (file_size < constants::SMALL_FILE_THRESHOLD) {
        return Codec::ZSTD;
    }
    
    // File grandi → LZMA (max ratio)
    return Codec::LZMA;
}

// ═══════════════════════════════════════════════════════════════════════════
// COMPRESSION
// ═══════════════════════════════════════════════════════════════════════════

ChunkResult compress_buffer(
    const std::vector<char>& input,
    Codec codec,
    int compression_level
) {
    ChunkResult result;
    result.raw_size = static_cast<uint32_t>(input.size());
    result.codec = codec;
    result.success = false;
    
    // Calcola checksum PRIMA della compressione
    result.checksum = security::compute_hash(input.data(), input.size());
    
    if (codec == Codec::STORE) {
        // Nessuna compressione
        result.data = input;
        result.success = true;
        return result;
    }
    
    if (codec == Codec::LZMA) {
        // Compressione LZMA
        size_t max_out = lzma_stream_buffer_bound(input.size());
        result.data.resize(max_out);
        
        size_t out_pos = 0;
        uint32_t preset = std::clamp(compression_level, 
                                     constants::MIN_COMPRESSION_LEVEL,
                                     constants::MAX_COMPRESSION_LEVEL);
        
        lzma_ret ret = lzma_easy_buffer_encode(
            preset | LZMA_PRESET_EXTREME,
            LZMA_CHECK_CRC64,
            nullptr,
            reinterpret_cast<const uint8_t*>(input.data()),
            input.size(),
            reinterpret_cast<uint8_t*>(result.data.data()),
            &out_pos,
            max_out
        );
        
        if (ret == LZMA_OK) {
            result.data.resize(out_pos);
            result.success = true;
        } else {
            // Fallback a STORE se LZMA fallisce
            result.data = input;
            result.codec = Codec::STORE;
            result.success = true;
        }
        
        return result;
    }
    
    // Codec non supportato → fallback STORE
    result.data = input;
    result.codec = Codec::STORE;
    result.success = true;
    
    return result;
}

} // namespace tarc::codec
