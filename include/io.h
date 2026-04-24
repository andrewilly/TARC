#pragma once
#include <string>
#include <vector>
#include <cstdio>
#include <cstdint>
#include <filesystem>
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

    FILE* get() const { return f; }
    operator FILE*() const { return f; }
    bool operator!() const { return !f; }
    explicit operator bool() const { return f != nullptr; }
};

// ─── IO NAMESPACE ──────────────────────────────────────────────────────────────
namespace IO {

    // ─── UNICODE-AWARE FOPEN ─────────────────────────────────────────────────
    // Su Windows converte a wide string e usa _wfopen().
    // Su POSIX usa fopen() direttamente (UTF-8 nativo).
    FILE* u8fopen(const std::string& utf8_path, const char* mode);

    // Aggiunge estensione .strk se mancante
    std::string ensure_ext(const std::string& path);

    // Espansione percorsi e wildcards (Unicode-aware cross-platform)
    void expand_path(const std::string& pattern, std::vector<std::string>& out);

    // ─── SICUREZZA PERCORSI ──────────────────────────────────────────────────

    // Sanitizza un percorso estratto dall'archivio (prevenzione path traversal)
    std::string sanitize_path(const std::string& path);

    // Valida header archivio (magic, versione, consistenza)
    bool validate_header(const Header& h);

    // Valida directory di output specificata dall'utente
    bool validate_output_dir(const std::string& dir);

    // ─── GESTIONE ARCHIVIO ───────────────────────────────────────────────────

    bool read_toc(FILE* f, Header& h, std::vector<FileEntry>& toc);
    bool write_toc(FILE* f, Header& h, std::vector<FileEntry>& toc);
    bool write_file_to_disk(const std::string& path, const char* data, size_t size, uint64_t timestamp);

    // ─── HELPERS I/O LOW LEVEL ───────────────────────────────────────────────

    bool read_bytes(FILE* f, void* buf, size_t size);
    bool write_bytes(FILE* f, const void* buf, size_t size);

    // ─── 64-BIT SEEK/TELL (cross-platform) ───────────────────────────────────

    bool seek64(FILE* f, int64_t offset, int origin);
    int64_t tell64(FILE* f);

    // ─── SCRITTURE ATOMICHE ──────────────────────────────────────────────────

    std::string make_temp_path(const std::string& target_path);
    bool atomic_rename(const std::string& from, const std::string& to);
    bool safe_remove(const std::string& path);

    // ─── CONVERSIONE TIMESTAMP PORTABILE ─────────────────────────────────────
    // Converte file_time_type → Unix timestamp (secondi dal 1970-01-01)
    // in modo portabile tra diversi epoch di file_time_type::clock
    uint64_t file_time_to_unix(std::filesystem::file_time_type ftime);

    // Converte Unix timestamp → file_time_type
    std::filesystem::file_time_type unix_to_file_time(uint64_t timestamp);

    // ─── UTILITA' ESEGUIBILE ─────────────────────────────────────────────────
    // Ritorna la directory contenente l'eseguibile corrente
    std::string get_exe_directory();

} // namespace IO
