#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "types.h"

namespace CodecSelector {
    Codec select(const std::string& path, size_t size, int level);
    bool is_compressibile(const std::string& ext);
}

namespace Engine {
    TarcResult compress(const std::string& arch_path,
                        const std::vector<std::string>& files,
                        bool append, int level,
                        const std::vector<std::string>& exclude_patterns = {});

    TarcResult extract(const std::string& arch_path,
                       const std::vector<std::string>& patterns = {},
                       bool test_only = false,
                       size_t offset = 0,
                       bool flat_mode = false,
                       const std::string& output_dir = "");

    TarcResult list(const std::string& arch_path, size_t offset = 0);

    TarcResult remove_files(const std::string& arch_path,
                            const std::vector<std::string>& patterns);

    TarcResult create_sfx(const std::string& archive_path, const std::string& sfx_name);
}