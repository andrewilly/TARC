#pragma once
#include <string>
#include <vector>
#include <cstdio>
#include <cstdint>
#include "types.h"

namespace IO {

    // CRC32 calcolo
    uint32_t calculate_crc32(const void* data, size_t size);
    uint32_t calculate_crc32_file(FILE* f, uint64_t offset, uint64_t size);
    
    // Verifica header
    bool verify_header_checksum(const Header& h);
    void update_header_checksum(Header& h);
    
    // Funzioni esistenti
    std::string ensure_ext(const std::string& path);
    void expand_path(const std::string& pattern, std::vector<std::string>& out);
    void resolve_wildcards(const std::string& pattern, std::vector<std::string>& out);
    
    bool read_toc(FILE* f, Header& h, std::vector<FileEntry>& toc);
    bool write_toc(FILE* f, Header& h, std::vector<FileEntry>& toc);
    
    bool write_file_to_disk(const std::string& path, 
                            const char* data, 
                            size_t size, 
                            uint64_t timestamp);
    
    bool read_bytes(FILE* f, void* buf, size_t size);
    bool write_bytes(FILE* f, const void* buf, size_t size);
    
    std::string normalize_path(std::string path);
    bool match_pattern(const std::string& full_path, const std::string& pattern);
}
