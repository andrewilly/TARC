#include "io.h"
#include "types.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <chrono>
#include <cstring>

namespace fs = std::filesystem;

namespace IO {

// Nota: implementa qui ensure_ext, expand_path, read_bytes ecc. 
// se non sono definiti altrove, altrimenti avrai errori di link.

bool read_toc(FILE* f, Header& h, std::vector<FileEntry>& toc) {
    if (fseek(f, h.toc_offset, SEEK_SET) != 0) return false;
    
    toc.clear();
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

bool write_file_to_disk(const std::string& path, const char* data, size_t size, uint64_t timestamp) {
    try {
        fs::path p(path);
        
        if (p.has_parent_path()) {
            fs::create_directories(p.parent_path());
        }

        std::ofstream out(path, std::ios::binary);
        if (!out) return false;
        
        if (size > 0 && data != nullptr) {
            out.write(data, size);
        }
        out.close();

        // Ripristino timestamp
        auto sys_time = std::chrono::system_clock::from_time_t(static_cast<time_t>(timestamp));
        fs::last_write_time(path, fs::file_time_type(sys_time.time_since_epoch()));
        
        return true;
    } catch (...) {
        return false;
    }
}

bool read_bytes(FILE* f, void* buf, size_t size) {
    return fread(buf, 1, size, f) == size;
}

bool write_bytes(FILE* f, const void* buf, size_t size) {
    return fwrite(buf, 1, size, f) == size;
}

bool write_entry(FILE* f, const FileEntry& entry) {
    if (fwrite(&entry.meta, sizeof(Entry), 1, f) != 1) return false;
    if (fwrite(entry.name.c_str(), 1, entry.meta.name_len, f) != entry.meta.name_len) return false;
    return true;
}

} // namespace IO
