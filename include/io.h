#pragma once
#include <string>
#include <vector>
#include <cstdio>
#include <cstdint>
#include "types.h"

// ─── RAII FILE* WRAPPER ────────────────────────────────────────────────────────
// Previene leak di file handle: fclose() automatico all'uscita dallo scope.
struct FilePtr {
    FILE* f = nullptr;

    explicit FilePtr(FILE* file) : f(file) {}
    ~FilePtr() { if (f) fclose(f); }

    FilePtr(const FilePtr&) = delete;
    FilePtr& operator=(const FilePtr&) = delete;

    FilePtr(FilePtr&& other) noexcept : f(other.f) { other.f = nullptr; }
    FilePtr& operator=(FilePtr&& other) noexcept {
        if (this != &other) {
            if (f) fclose(f);
            f = other.f;
            other.f = nullptr;
        }
        return *this;
    }

    operator FILE*() const { return f; }
    bool operator!() const { return !f; }
    explicit operator bool() const { return f != nullptr; }
};

// ─── IO NAMESPACE ──────────────────────────────────────────────────────────────
namespace IO {

    // ─── INTERVENTO #19: UNICODE-AWARE FOPEN ───────────────────────────────────
    // Apre un file con percorso UTF-8. Su Windows converte internamente
    // a wide string e usa _wfopen(). Su POSIX usa fopen() direttamente.
    FILE* u8fopen(const std::string& utf8_path, const char* mode);

    // Aggiunge estensione se mancante (.strk)
    std::string ensure_ext(const std::string& path);

    // Espansione percorsi e wildcards (Unicode-aware cross-platform)
    void expand_path(const std::string& pattern, std::vector<std::string>& out);

    // --- SICUREZZA PERCORSI (Intervento #11) ---

    std::string sanitize_path(const std::string& path);
    bool validate_header(const Header& h);

    // --- GESTIONE ARCHIVIO ---

    bool read_toc(FILE* f, Header& h, std::vector<FileEntry>& toc);
    bool write_toc(FILE* f, Header& h, std::vector<FileEntry>& toc);
    bool write_file_to_disk(const std::string& path, const char* data, size_t size, uint64_t timestamp);

    // --- HELPERS I/O LOW LEVEL ---

    bool read_bytes(FILE* f, void* buf, size_t size);
    bool write_bytes(FILE* f, const void* buf, size_t size);

    // --- 64-BIT SEEK/TELL (cross-platform) ---

    bool seek64(FILE* f, int64_t offset, int origin);
    int64_t tell64(FILE* f);

    // --- SCRITTURE ATOMICHE (Intervento #12) ---

    std::string make_temp_path(const std::string& target_path);
    bool atomic_rename(const std::string& from, const std::string& to);
    bool safe_remove(const std::string& path);

} // namespace IO
