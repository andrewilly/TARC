#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "types.h"

namespace CodecSelector {
    Codec select(const std::string& path, size_t size);
    bool is_compressible(const std::string& ext);
}

namespace Engine {

    struct FileMeta {
        uint64_t orig_size;
        uint64_t timestamp;
        uint64_t xxhash;
        uint8_t  codec;
        bool     is_duplicate = false;
        uint32_t duplicate_of_idx = 0;
    };

    struct FileEntryInternal {
        std::string name;
        FileMeta meta;
        std::string extension;
    };

    // Funzioni Core Release 2.0
    TarcResult compress(const std::string& arch_path, const std::vector<std::string>& files, bool append, int level);
    TarcResult extract(const std::string& arch_path, const std::vector<std::string>& patterns = {}, bool test_only = false, size_t offset = 0);
    TarcResult list(const std::string& arch_path, size_t offset = 0);
    TarcResult remove_files(const std::string& arch_path, const std::vector<std::string>& patterns);
    
    // Specifica per generazione Autoestraente
    TarcResult create_sfx(const std::string& archive_path, const std::string& sfx_name);

} // namespace Engine
