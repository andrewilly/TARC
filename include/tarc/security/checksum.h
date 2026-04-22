#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include "tarc/util/result.h"

namespace tarc::security {

// ═══════════════════════════════════════════════════════════════════════════
// XXH64 WRAPPER
// ═══════════════════════════════════════════════════════════════════════════

/// Calcola XXH64 di un buffer
[[nodiscard]] uint64_t compute_hash(const void* data, size_t size);

/// Calcola XXH64 di un file
[[nodiscard]] Result compute_file_hash(const std::string& path, uint64_t& hash_out);

// ═══════════════════════════════════════════════════════════════════════════
// CHECKSUM VERIFICATION
// ═══════════════════════════════════════════════════════════════════════════

/// Verifica checksum di un chunk decompresso
[[nodiscard]] Result verify_chunk_checksum(
    const void* data, 
    size_t size, 
    uint64_t expected_hash
);

/// Verifica integrità file usando XXH64
[[nodiscard]] Result verify_file_integrity(
    const std::string& path,
    uint64_t expected_hash
);

} // namespace tarc::security
