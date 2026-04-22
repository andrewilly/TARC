#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "types.h"

namespace CodecSelector {
    Codec select(const std::string& path, size_t size);
    bool is_compressibile(const std::string& ext);
    bool should_skip_dedup(const std::string& path);
}

namespace Engine {
    TarcResult compress(const std::string& arch_path, const std::vector<std::string>& files, bool append, int level);
    TarcResult extract(const std::string& arch_path, const std::vector<std::string>& patterns = {}, bool test_only = false, size_t offset = 0, bool flat_mode = false);
    TarcResult list(const std::string& arch_path, size_t offset = 0);
    TarcResult remove_files(const std::string& arch_path, const std::vector<std::string>& patterns);
    TarcResult create_sfx(const std::string& archive_path, const std::string& sfx_name);
    void set_skip_dedup_extensions(const std::vector<std::string>& exts);
    void set_chunk_threshold(size_t threshold);
}
