#include "io.h"
#include "types.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <chrono>
#include <cstring>

namespace fs = std::filesystem;

namespace IO {

std::string ensure_ext(const std::string& path) {
    if (path.length() < 5 || path.substr(path.length() - 5) != ".strk") {
        return path + ".strk";
    }
    return path;
}

void expand_path(const std::string& pattern, std::vector<std::string>& out) {
    // Non facciamo più espansione qui, la facciamo nell'Engine!
    // Aggiungiamo il pattern così com'è alla lista dei target.
    if (!pattern.empty()) {
        out.push_back(pattern);
    }
}

bool read_toc(FILE* f, Header& h, std::vector<FileEntry>& toc) {
    if (h.toc_offset == 0) return false;
    if (fseek(f, (long)h.toc_offset, SEEK_SET) != 0) return false;
    
    toc.clear();
    for (uint32_t i = 0; i < h.file_count; ++i) {
        FileEntry fe;
        if (fread(&fe.meta, sizeof(Entry), 1, f) != 1) return false;
        
        std::vector<char> name_buf(fe.meta.name_len + 1, 0);
        if (fread(name_buf.data(), 1, fe.meta.name_len, f) != fe.meta.name_len) return false;
        fe.name = std::string(name_buf.data());
        toc.push_back(fe);
    }
    return true;
}

bool write_toc(FILE* f, Header& h, std::vector<FileEntry>& toc) {
    // Assicura che tutti i dati siano scritti
    fflush(f);
    
    // Salva posizione corrente
    long toc_pos = ftell(f);
    if (toc_pos == -1) return false;
    
    h.toc_offset = (uint64_t)toc_pos;
    h.file_count = static_cast<uint32_t>(toc.size());

    // Scrivi tutte le entry
    for (auto& fe : toc) {
        fe.meta.name_len = static_cast<uint16_t>(fe.name.length());
        if (fwrite(&fe.meta, sizeof(Entry), 1, f) != 1) return false;
        if (fwrite(fe.name.c_str(), 1, fe.meta.name_len, f) != fe.meta.name_len) return false;
    }

    // Torna all'inizio e aggiorna header
    if (fseek(f, 0, SEEK_SET) != 0) return false;
    if (fwrite(&h, sizeof(Header), 1, f) != 1) return false;
    
    // Torna alla fine del file
    if (fseek(f, 0, SEEK_END) != 0) return false;
    fflush(f);
    return true;
}

bool write_file_to_disk(const std::string& path, const char* data, size_t size, uint64_t timestamp) {
    try {
        fs::path p(path);
        if (p.has_parent_path()) fs::create_directories(p.parent_path());
        std::ofstream out(path, std::ios::binary);
        if (!out) return false;
        if (size > 0 && data != nullptr) out.write(data, size);
        out.close();

        auto sys_time = std::chrono::system_clock::from_time_t((time_t)timestamp);
        fs::last_write_time(p, fs::file_time_type(sys_time.time_since_epoch()));
        return true;
    } catch (...) { return false; }
}

bool read_bytes(FILE* f, void* buf, size_t size) { return fread(buf, 1, size, f) == size; }
bool write_bytes(FILE* f, const void* buf, size_t size) { return fwrite(buf, 1, size, f) == size; }

} // namespace IO
