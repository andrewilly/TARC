#include "tarc/security/checksum.h"
#include "tarc/util/constants.h"
#include <fstream>

extern "C" {
    #include "xxhash.h"
}

namespace tarc::security {

// ═══════════════════════════════════════════════════════════════════════════
// XXH64 WRAPPER
// ═══════════════════════════════════════════════════════════════════════════

uint64_t compute_hash(const void* data, size_t size) {
    return XXH64(data, size, 0);
}

Result compute_file_hash(const std::string& path, uint64_t& hash_out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return Result::error(ErrorCode::CannotOpenFile, 
            "Impossibile aprire file: " + path);
    }
    
    XXH64_state_t* state = XXH64_createState();
    if (!state) {
        return Result::error(ErrorCode::OutOfMemory, 
            "Impossibile allocare stato XXH64");
    }
    
    XXH64_reset(state, 0);
    
    std::vector<char> buffer(constants::IO_BUFFER_SIZE);
    while (file) {
        file.read(buffer.data(), buffer.size());
        std::streamsize bytes_read = file.gcount();
        if (bytes_read > 0) {
            XXH64_update(state, buffer.data(), bytes_read);
        }
    }
    
    hash_out = XXH64_digest(state);
    XXH64_freeState(state);
    
    return Result::success();
}

// ═══════════════════════════════════════════════════════════════════════════
// CHECKSUM VERIFICATION
// ═══════════════════════════════════════════════════════════════════════════

Result verify_chunk_checksum(const void* data, size_t size, uint64_t expected_hash) {
    uint64_t actual_hash = compute_hash(data, size);
    
    if (actual_hash != expected_hash) {
        return Result::error(ErrorCode::ChecksumMismatch,
            "Checksum chunk non valido (atteso: 0x" + 
            std::to_string(expected_hash) + ", trovato: 0x" + 
            std::to_string(actual_hash) + ")");
    }
    
    return Result::success();
}

Result verify_file_integrity(const std::string& path, uint64_t expected_hash) {
    uint64_t actual_hash = 0;
    auto result = compute_file_hash(path, actual_hash);
    
    if (result.failed()) {
        return result;
    }
    
    if (actual_hash != expected_hash) {
        return Result::error(ErrorCode::ChecksumMismatch,
            "Integrità file compromessa: " + path);
    }
    
    return Result::success();
}

} // namespace tarc::security
