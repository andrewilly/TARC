#pragma once
#include <string>
#include <vector>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <optional>
#include "types.h"

// 64-bit file positioning: cross-platform helpers
#ifdef _WIN32
    // _fseeki64 / _ftelli64 are always 64-bit on Windows
#else
    #include <unistd.h>  // for off_t (64-bit on 64-bit POSIX)
#endif

namespace IO {

    // 64-bit file seek: replaces fseek(f, (long)offset, origin)
    inline int tarc_fseek(FILE* f, int64_t offset, int origin) {
#ifdef _WIN32
        return _fseeki64(f, offset, origin);
#else
        return fseeko(f, static_cast<off_t>(offset), origin);
#endif
    }

    // 64-bit file tell: replaces ftell(f)
    inline int64_t tarc_ftell(FILE* f) {
#ifdef _WIN32
        return _ftelli64(f);
#else
        return static_cast<int64_t>(ftello(f));
#endif
    }

    std::string ensure_ext(const std::string& path);

    bool expand_path(const std::string& pattern, std::vector<std::string>& out);

    bool read_toc(FILE* f, Header& h, std::vector<FileEntry>& toc);

    bool write_toc(FILE* f, Header& h, std::vector<FileEntry>& toc);

    // SEC-007: overwrite + ARCH-008: output_dir support
    bool write_file_to_disk(const std::string& path, const char* data, size_t size,
                            uint64_t timestamp, bool overwrite = false,
                            const std::string& output_dir = "");

    bool read_bytes(FILE* f, void* buf, size_t size);

    bool write_bytes(FILE* f, const void* buf, size_t size);

    bool write_entry(FILE* f, const FileEntry& entry);

    Result<FileEntry> read_entry(FILE* f);

    // === Security functions ===

    bool validate_archive_header(const Header& h);
    std::string sanitize_extract_path(const std::string& entry_name);
    bool is_safe_filename(const std::string& name);
    bool file_exists(const std::string& path);

}
