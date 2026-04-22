#pragma once
#include <string>
#include <vector>
#include <cstdio>
#include <cstdint>
#include "types.h"

namespace IO {

    [[nodiscard]] std::string ensure_ext(const std::string& path);
    
    void expand_path(const std::string& pattern, std::vector<std::string>& out);
    void resolve_wildcards(const std::string& pattern, std::vector<std::string>& out);
    
    // Lettura/scrittura TOC
    [[nodiscard]] bool read_toc(FILE* f, Header& h, std::vector<FileEntry>& toc);
    [[nodiscard]] bool write_toc(FILE* f, Header& h, std::vector<FileEntry>& toc);
    
    // Scrittura file su disco con timestamp
    [[nodiscard]] bool write_file_to_disk(const std::string& path, 
                                           const char* data, 
                                           size_t size, 
                                           uint64_t timestamp);
    
    // Helper I/O
    [[nodiscard]] bool read_bytes(FILE* f, void* buf, size_t size);
    [[nodiscard]] bool write_bytes(FILE* f, const void* buf, size_t size);
    
    // Normalizzazione percorso
    [[nodiscard]] std::string normalize_path(std::string path);
    
    // Matching pattern con wildcard
    [[nodiscard]] bool match_pattern(const std::string& full_path, const std::string& pattern);
}
