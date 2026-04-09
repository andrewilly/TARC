#include "io.h"
#include <cstring>
#include <filesystem>
#include <algorithm>

namespace fs = std::filesystem;

namespace IO {

bool read_bytes(FILE* f, void* buf, size_t size) {
    if (!f) return false;
    return fread(buf, 1, size, f) == size;
}

bool write_bytes(FILE* f, const void* buf, size_t size) {
    if (!f) return false;
    return fwrite(buf, 1, size, f) == size;
}

std::string ensure_ext(const std::string& path) {
    const std::string ext = TARC_EXT;
    if (path.size() < ext.size() ||
        path.compare(path.size() - ext.size(), ext.size(), ext) != 0)
        return path + ext;
    return path;
}

void expand_path(const std::string& pattern, std::vector<std::string>& out) {
    fs::path p(pattern);
    if (fs::exists(p)) {
        if (fs::is_directory(p)) {
            for (auto const& entry : fs::recursive_directory_iterator(p)) {
                if (fs::is_regular_file(entry)) out.push_back(entry.path().string());
            }
        } else {
            out.push_back(p.string());
        }
        return;
    }
    
    fs::path dir = p.has_parent_path() ? p.parent_path() : fs::current_path();
    std::string filename = p.filename().string();
    
    if (filename.find('*') != std::string::npos) {
        std::string suffix = filename.substr(filename.find('*') + 1);
        if (fs::exists(dir)) {
            for (auto const& entry : fs::directory_iterator(dir)) {
                std::string ename = entry.path().filename().string();
                if (suffix.empty() || (ename.size() >= suffix.size() && 
                    ename.compare(ename.size() - suffix.size(), suffix.size(), suffix) == 0)) {
                    out.push_back(entry.path().string());
                }
            }
        }
    }
}

bool read_toc(FILE* f, Header& h, std::vector<FileEntry>& toc) {
    if (!f) return false;
    rewind(f);
    if (!read_bytes(f, &h, sizeof(Header))) return false;

    // Verifica Magic Number
    if (memcmp(h.magic, TARC_MAGIC, 4) != 0) return false;

    // Retrocompatibilità: Accetta versione 103 e 104
    if (h.version < 103) return false;

    if (fseek(f, (long)h.toc_offset, SEEK_SET) != 0) return false;

    uint32_t count = 0;
    if (!read_bytes(f, &count, sizeof(count))) return false;

    toc.clear();
    toc.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        FileEntry fe;
        if (!read_bytes(f, &fe.meta, sizeof(Entry))) break;
        fe.name.resize(fe.meta.name_len);
        if (!read_bytes(f, fe.name.data(), fe.meta.name_len)) break;
        toc.push_back(std::move(fe));
    }
    return true;
}

void write_toc(FILE* f, Header& h, std::vector<FileEntry>& toc) {
    if (!f) return;
    
    h.version = TARC_VERSION; 
    h.toc_offset = (uint64_t)ftell(f);
    uint32_t count = (uint32_t)toc.size();
    write_bytes(f, &count, sizeof(count));
    
    for (auto& fe : toc) {
        fe.meta.name_len = (uint16_t)fe.name.size();
        write_bytes(f, &fe.meta, sizeof(Entry));
        write_bytes(f, fe.name.data(), fe.name.size());
    }
    
    rewind(f);
    write_bytes(f, &h, sizeof(Header));
}

} // namespace IO
