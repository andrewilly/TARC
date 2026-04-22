#pragma once
#include <string>
#include <vector>
#include <cstdio>
#include <cstdint>
#include "types.h"

// ─── RAII FILE* WRAPPER ────────────────────────────────────────────────────────
// Previene leak di file handle: fclose() automatico al uscita dallo scope.
// Sostituisce direttamente FILE* ovunque, zero overhead runtime.
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

    // Aggiunge estensione se mancante (.strk)
    std::string ensure_ext(const std::string& path);

    // Espansione percorsi e wildcards (Windows/Linux)
    void expand_path(const std::string& pattern, std::vector<std::string>& out);

    // --- GESTIONE ARCHIVIO ---

    // Legge Header + TOC: Carica la lista file alla fine dell'archivio
    bool read_toc(FILE* f, Header& h, std::vector<FileEntry>& toc);

    // Scrive TOC e aggiorna Header: Chiamata alla fine del processo di compressione
    bool write_toc(FILE* f, Header& h, std::vector<FileEntry>& toc);

    // Scrive un file su disco con gestione cartelle e timestamp
    bool write_file_to_disk(const std::string& path, const char* data, size_t size, uint64_t timestamp);

    // --- HELPERS I/O LOW LEVEL ---

    // Helpers per byte raw con controlli di errore rigorosi
    bool read_bytes(FILE* f, void* buf, size_t size);
    bool write_bytes(FILE* f, const void* buf, size_t size);

    // Scrittura singola entry nel TOC
    bool write_entry(FILE* f, const FileEntry& entry);

    // --- 64-BIT SEEK/TELL (cross-platform) ---
    // Usa _fseeki64/_ftelli64 su Windows, fseeko/ftello su Linux/macOS
    // Necessario per archivi > 2GB dove long e' a 32 bit

    bool seek64(FILE* f, int64_t offset, int origin);
    int64_t tell64(FILE* f);

} // namespace IO
