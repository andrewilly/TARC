#pragma once
#include <string>
#include <vector>
#include <cstdio>
#include "types.h"

namespace IO {

    // Aggiunge estensione se mancante
    std::string ensure_ext(const std::string& path);

    // Espansione percorsi e wildcards
    void expand_path(const std::string& pattern, std::vector<std::string>& out);

    // Legge Header + TOC
    bool read_toc(FILE* f, Header& h, std::vector<FileEntry>& toc);

    // Scrive TOC e aggiorna Header (Restituisce bool per gestione errori)
    bool write_toc(FILE* f, Header& h, std::vector<FileEntry>& toc);

    // Scrive un file su disco con gestione cartelle e timestamp (Fase 3)
    bool write_file_to_disk(const std::string& path, const char* data, size_t size, uint64_t timestamp);

    // Helpers per byte raw
    bool read_bytes(FILE* f, void* buf, size_t size);
    bool write_bytes(FILE* f, const void* buf, size_t size);

    // Scrittura singola entry
    bool write_entry(FILE* f, const FileEntry& entry);

} // namespace IO
