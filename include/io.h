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

    // Aggiunge estensione se mancante (.strk)
    std::string ensure_ext(const std::string& path);

    // Espansione percorsi e wildcards (Windows/Linux)
    void expand_path(const std::string& pattern, std::vector<std::string>& out);

    // --- SICUREZZA PERCORSI (Intervento #11) ---

    // Sanitizza un percorso per prevenire Path Traversal.
    // Rimuove componenti "..", percorsi assoluti, e caratteri pericolosi.
    // Restituisce stringa vuota se il percorso e' irrimediabilmente pericoloso.
    std::string sanitize_path(const std::string& path);

    // Valida che un header sia un archivio TARC valido (magic + versione)
    // Intervento #13: usato in append per rifiutare file non-TARC
    bool validate_header(const Header& h);

    // --- GESTIONE ARCHIVIO ---

    // Legge Header + TOC
    bool read_toc(FILE* f, Header& h, std::vector<FileEntry>& toc);

    // Scrive TOC e aggiorna Header
    bool write_toc(FILE* f, Header& h, std::vector<FileEntry>& toc);

    // Scrive un file su disco con gestione cartelle e timestamp
    bool write_file_to_disk(const std::string& path, const char* data, size_t size, uint64_t timestamp);

    // --- HELPERS I/O LOW LEVEL ---

    bool read_bytes(FILE* f, void* buf, size_t size);
    bool write_bytes(FILE* f, const void* buf, size_t size);

    // --- 64-BIT SEEK/TELL (cross-platform) ---

    bool seek64(FILE* f, int64_t offset, int origin);
    int64_t tell64(FILE* f);

    // --- SCRITTURE ATOMICHE (Intervento #12) ---

    // Genera un nome file temporaneo unico nella stessa directory del path target.
    // Formato: <basename>.strk.tmp<random_hex>
    std::string make_temp_path(const std::string& target_path);

    // Rinomina atomicamente un file. Su Windows usa MoveFileExA,
    // su POSIX usa rename() (atomico sullo stesso filesystem).
    bool atomic_rename(const std::string& from, const std::string& to);

    // Rimuove un file in modo sicuro (per cleanup in caso di errore).
    bool safe_remove(const std::string& path);

} // namespace IO
