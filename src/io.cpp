#include "io.h"
#include "types.h"
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace IO {

bool read_toc(FILE* f, Header& h, std::vector<FileEntry>& toc) {
    if (fseek(f, h.toc_offset, SEEK_SET) != 0) return false;
    
    for (uint32_t i = 0; i < h.file_count; ++i) {
        FileEntry fe;
        if (fread(&fe.meta, sizeof(Entry), 1, f) != 1) return false;
        
        std::vector<char> name_buf(fe.meta.name_len);
        if (fread(name_buf.data(), 1, fe.meta.name_len, f) != fe.meta.name_len) return false;
        fe.name = std::string(name_buf.begin(), name_buf.end());
        
        toc.push_back(fe);
    }
    return true;
}

bool write_toc(FILE* f, Header& h, std::vector<FileEntry>& toc) {
    h.toc_offset = ftell(f);
    h.file_count = static_cast<uint32_t>(toc.size());

    for (auto& fe : toc) {
        fe.meta.name_len = static_cast<uint16_t>(fe.name.length());
        if (fwrite(&fe.meta, sizeof(Entry), 1, f) != 1) return false;
        if (fwrite(fe.name.c_str(), 1, fe.meta.name_len, f) != fe.meta.name_len) return false;
    }

    fseek(f, 0, SEEK_SET);
    if (fwrite(&h, sizeof(Header), 1, f) != 1) return false;
    
    return true;
}

// Funzione helper per creare cartelle e scrivere file
bool write_file_to_disk(const std::string& path, const char* data, size_t size, uint64_t timestamp) {
    try {
        fs::path p(path);
        if (p.has_parent_path()) {
            fs::create_directories(p.parent_path());
        }

        std::ofstream out(path, std::ios::binary);
        if (!out) return false;
        out.write(data, size);
        out.close();

        // Ripristina timestamp
        fs::last_write_time(path, fs::file_time_type(std::chrono::seconds(timestamp)));
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace IO
