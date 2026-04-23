#pragma once
#include <string>
#include <vector>
#include <cstdio>
#include <cstdint>
#include "types.h"

// ─── RAII FILE* WRAPPER ────────────────────────────────────────────────────────
struct FilePtr {
    FILE* f = nullptr;

    explicit FilePtr(FILE* file) : f(file) {}
    ~FilePtr() { reset(); }

    FilePtr(const FilePtr&) = delete;
    FilePtr& operator=(const FilePtr&) = delete;

    FilePtr(FilePtr&& other) noexcept : f(other.f) { other.f = nullptr; }
    FilePtr& operator=(FilePtr&& other) noexcept {
        if (this != &other) {
            reset();
            f = other.f;
            other.f = nullptr;
        }
        return *this;
    }

    void reset() {
        if (f) {
            fclose(f);
            f = nullptr;
        }
    }

    operator FILE*() const { return f; }
    bool operator!() const { return !f; }
    explicit operator bool() const { return f != nullptr; }
};

namespace IO {
    FILE* u8fopen(const std::string& utf8_path, const char* mode);
    std::string ensure_ext(const std::string& path);
    void expand_path(const std::string& pattern, std::vector<std::string>& out);
    
    // Sicurezza migliorata
    std::string sanitize_path(const std::string& path);
    bool validate_header(const Header& h);

    bool read_toc(FILE* f, Header& h, std::vector<FileEntry>& toc);
    bool write_toc(FILE* f, Header& h, std::vector<FileEntry>& toc);
    bool write_file_to_disk(const std::string& path, const char* data, size_t size, uint64_t timestamp);

    bool read_bytes(FILE* f, void* buf, size_t size);
    bool write_bytes(FILE* f, const void* buf, size_t size);

    bool seek64(FILE* f, int64_t offset, int whence);
    int64_t tell64(FILE* f);

    bool atomic_rename(const std::string& old_p, const std::string& new_p);
    void safe_remove(const std::string& path);
}