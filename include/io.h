#pragma once
#include <string>
#include <vector>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <optional>
#include "types.h"

namespace fs = std::filesystem;

namespace IO {

    std::string ensure_ext(const std::string& path);

    // Unicode-aware file operations
    FILE* u8fopen(const std::string& path, const std::string& mode);
    bool seek64(FILE* f, int64_t offset, int origin);
    bool validate_header(const Header& h);
    std::string make_temp_path(const std::string& path);
    bool safe_remove(const std::string& path);
bool atomic_rename(const std::string& from, const std::string& to);

    std::string sanitize_path(const std::string& path);

    bool expand_path(const std::string& pattern, std::vector<std::string>& out);

    bool read_toc(FILE* f, Header& h, std::vector<FileEntry>& toc);

    bool write_toc(FILE* f, Header& h, std::vector<FileEntry>& toc);

    bool write_file_to_disk(const std::string& path, const char* data, size_t size, uint64_t timestamp);

    bool read_bytes(FILE* f, void* buf, size_t size);

    bool write_bytes(FILE* f, const void* buf, size_t size);

    bool write_entry(FILE* f, const FileEntry& entry);

    Result<FileEntry> read_entry(FILE* f);

}
