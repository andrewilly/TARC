#include "io.h"
#include <filesystem>
#include <algorithm>

namespace fs = std::filesystem;

namespace IO {

// Funzione aggiornata per il nuovo standard .strk
std::string ensure_ext(const std::string& filename) {
    std::string ext = ".strk";
    if (filename.size() < ext.size() || 
        filename.compare(filename.size() - ext.size(), ext.size(), ext) != 0) {
        return filename + ext;
    }
    return filename;
}

bool read_bytes(FILE* f, void* buffer, size_t size) {
    if (!f || !buffer) return false;
    return fread(buffer, 1, size, f) == size;
}

bool write_bytes(FILE* f, const void* buffer, size_t size) {
    if (!f || !buffer) return false;
    return fwrite(buffer, 1, size, f) == size;
}

// Aggiornata per gestire correttamente l'header STRK
bool read_toc(FILE* f, Header& h, std::vector<FileEntry>& toc) {
    fseek(f, 0, SEEK_SET);
    if (!read_bytes(f, &h, sizeof(Header))) return false;

    // Se il magic non è STRK, è un file incompatibile
    if (std::memcmp(h.magic, "STRK", 4) != 0) return false;

    fseek(f, h.toc_offset, SEEK_SET);
    for (uint32_t i = 0; i < h.file_count; ++i) {
        FileEntry fe;
        uint32_t name_len;
        if (!read_bytes(f, &name_len, sizeof(name_len))) return false;
        
        std::vector<char> name_buf(name_len);
        if (!read_bytes(f, name_buf.data(), name_len)) return false;
        fe.name.assign(name_buf.begin(), name_buf.end());
        
        if (!read_bytes(f, &fe.meta, sizeof(FileMeta))) return false;
        toc.push_back(fe);
    }
    return true;
}

void write_toc(FILE* f, Header& h, const std::vector<FileEntry>& toc) {
    h.toc_offset = ftell(f);
    h.file_count = (uint32_t)toc.size();

    for (const auto& fe : toc) {
        uint32_t name_len = (uint32_t)fe.name.size();
        write_bytes(f, &name_len, sizeof(name_len));
        write_bytes(f, fe.name.data(), name_len);
        write_bytes(f, &fe.meta, sizeof(FileMeta));
    }

    // Aggiorna l'header all'inizio del file con l'offset finale della TOC
    fseek(f, 0, SEEK_SET);
    write_bytes(f, &h, sizeof(Header));
}

void expand_path(const std::string& path, std::vector<std::string>& files) {
    if (fs::is_regular_file(path)) {
        files.push_back(path);
    } else if (fs::is_directory(path)) {
        for (const auto& entry : fs::recursive_directory_iterator(path)) {
            if (fs::is_regular_file(entry)) {
                files.push_back(entry.path().string());
            }
        }
    }
}

} // namespace IO
