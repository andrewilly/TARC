#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "types.h"

namespace CodecSelector {
    Codec select(const std::string& path, int level);
    bool is_high_entropy(const uint8_t* data, size_t len);
}

namespace Engine {

    // Definiamo la nostra struttura Meta interna per essere sicuri che abbia i campi
    struct FileMeta {
        uint64_t orig_size;
        uint64_t timestamp;
        uint64_t xxhash;
        uint8_t  codec;
        bool     is_duplicate = false;      // <--- IL CAMPO MANCANTE
        uint32_t duplicate_of_idx = 0;      // <--- IL CAMPO MANCANTE
    };

    struct FileEntry {
        std::string name;
        FileMeta meta;
    };

    TarcResult compress(const std::string& arch_path, const std::vector<std::string>& files, bool append, int level);
    TarcResult extract(const std::string& arch_path, bool test_only);
    TarcResult list(const std::string& arch_path);
    TarcResult remove_files(const std::string& arch_path, const std::vector<std::string>& patterns);

} // namespace Engine
