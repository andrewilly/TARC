#include "io.h"
#include <filesystem>
#include <algorithm>
#include <cstring> // Per std::memcmp

namespace fs = std::filesystem;

namespace IO {

std::string ensure_ext(const std::string& filename) {
    std::string ext = TARC_EXT;
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

bool read_toc(FILE* f, Header& h, std::vector<FileEntry>& toc) {
    fseek(f, 0, SEEK_SET);
    if (!read_bytes(f, &h, sizeof(Header))) return false;

    // Verifica Magic Number
    if (std::memcmp(h.magic, TARC_MAGIC, 4) != 0) return false;

    fseek(f, h.toc_offset, SEEK_SET);
    for (uint32_t i = 0; i < h.file_count; ++i) {
        FileEntry fe;
        uint32_t name_len;
        
        // Leggiamo la lunghezza del nome
        if (!read_bytes(f, &name_len, sizeof(name_len))) return false;
        
        // Leggiamo il nome del file
        std::vector<char> name_buf(name_len);
        if (!read_bytes(f, name_buf.data(), name_len)) return false;
        fe.name.assign(name_buf.begin(), name_buf.end());
        
        // Leggiamo i metadati (Entry)
        if (!read_bytes(f, &fe.meta, sizeof(Entry))) return false;
        
        toc.push_back(fe);
    }
    return true;
}

void write_toc(FILE* f, Header& h, std::vector<FileEntry>& toc) {
    h.toc_offset = ftell(f);
    h.file_count = (uint32_t)toc.size();

    for (const auto& fe : toc) {
        uint32_t name_len = (uint32_t)fe.name.size();
        // Scriviamo lunghezza nome, nome e metadati
        write_bytes(f, &name_len, sizeof(name_len));
        write_bytes(f, fe.name.data(), name_len);
        write_bytes(f, &fe.meta, sizeof(Entry));
    }

    // Aggiorniamo l'header all'inizio del file
    fseek(f, 0, SEEK_SET);
    write_bytes(f, &h, sizeof(Header));
}

void expand_path(const std::string& path, std::vector<std::string>& files) {
    if (fs::exists(path)) {
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
}

} // namespace IO
