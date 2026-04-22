#pragma once
#include <cstddef>
#include <cstdint>

namespace tarc::constants {

// ═══════════════════════════════════════════════════════════════════════════
// ARCHIVIO
// ═══════════════════════════════════════════════════════════════════════════

inline constexpr char MAGIC[4] = {'T', 'R', 'C', '2'};
inline constexpr uint32_t VERSION = 200;
inline constexpr char EXTENSION[] = ".strk";

// ═══════════════════════════════════════════════════════════════════════════
// LIMITI DI SICUREZZA (Protezione da archivi malformati)
// ═══════════════════════════════════════════════════════════════════════════

inline constexpr size_t MAX_CHUNK_SIZE = 512 * 1024 * 1024;      // 512 MB
inline constexpr size_t SOLID_CHUNK_SIZE = 256 * 1024 * 1024;    // 256 MB
inline constexpr size_t MAX_FILENAME_LENGTH = 1024;              // 1 KB
inline constexpr size_t MAX_FILE_COUNT = 10'000'000;             // 10M file
inline constexpr size_t IO_BUFFER_SIZE = 1 * 1024 * 1024;        // 1 MB

// ═══════════════════════════════════════════════════════════════════════════
// TIMESTAMP (Range validazione: 1970-2100)
// ═══════════════════════════════════════════════════════════════════════════

inline constexpr uint64_t MIN_TIMESTAMP = 0;                     // 1970-01-01
inline constexpr uint64_t MAX_TIMESTAMP = 4102444800;            // 2100-01-01

// ═══════════════════════════════════════════════════════════════════════════
// COMPRESSIONE
// ═══════════════════════════════════════════════════════════════════════════

inline constexpr int DEFAULT_COMPRESSION_LEVEL = 3;
inline constexpr int MIN_COMPRESSION_LEVEL = 1;
inline constexpr int MAX_COMPRESSION_LEVEL = 9;

inline constexpr size_t SMALL_FILE_THRESHOLD = 512 * 1024;       // 512 KB

// ═══════════════════════════════════════════════════════════════════════════
// PERFORMANCE
// ═══════════════════════════════════════════════════════════════════════════

inline constexpr size_t VECTOR_RESERVE_COUNT = 1024;             // Pre-alloc
inline constexpr size_t HASH_MAP_RESERVE = 10000;                // Dedup map

} // namespace tarc::constants
