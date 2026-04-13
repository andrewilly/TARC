#pragma once
#include <string>
#include <vector>
#include <cstdio>
#include <cstdint>
#include "types.h"

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
    // Cruciale per ripristinare correttamente i file estratti
    bool write_file_to_disk(const std::string& path, const char* data, size_t size, uint64_t timestamp);

    // --- HELPERS I/O LOW LEVEL ---

    // Helpers per byte raw con controlli di errore rigorosi
    bool read_bytes(FILE* f, void* buf, size_t size);
    bool write_bytes(FILE* f, const void* buf, size_t size);

    // Scrittura singola entry nel TOC
    bool write_entry(FILE* f, const FileEntry& entry);

    // --- UTILITIES PER SOLID MODE (Aggiunte per coerenza con Engine) ---
    
    // Legge un intero file in un buffer (usata in fase di chunking)
    // std::vector<char> read_file_to_buffer(const std::string& path);

} // namespace IO
