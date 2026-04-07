#pragma once
#include <string>
#include <vector>
#include "types.h"

namespace IO {

    // Aggiunge .tar4 se mancante
    std::string ensure_ext(const std::string& path);

    // Espande un pattern (con wildcards) in lista di file.
    // Se path è una directory, la visita ricorsivamente.
    void expand_path(const std::string& pattern, std::vector<std::string>& out);

    // Legge Header + TOC da un archivio aperto.
    // Restituisce false se il file non è un archivio TARC valido.
    bool read_toc(FILE* f, Header& h, std::vector<FileEntry>& toc);

    // Scrive TOC e aggiorna Header nel file.
    void write_toc(FILE* f, Header& h, std::vector<FileEntry>& toc);

    // Lettura/scrittura helpers
    bool read_bytes(FILE* f, void* buf, size_t size);
    bool write_bytes(FILE* f, const void* buf, size_t size);

} // namespace IO
