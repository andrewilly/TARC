#include "io.h"
#include "types.h"
#include "ui.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <chrono>
#include <cstring>

#ifdef _WIN32
    #include <windows.h>
#endif

namespace fs = std::filesystem;

namespace IO {

std::string ensure_ext(const std::string& path) {
    if (path.length() < 5 || path.substr(path.length() - 5) != ".strk") {
        return path + ".strk";
    }
    return path;
}

bool read_toc(FILE* f, Header& h, std::vector<FileEntry>& toc) {
    if (h.toc_offset == 0) return false;
    
#ifdef _WIN32
    if (_fseeki64(f, (__int64)h.toc_offset, SEEK_SET) != 0) return false;
#else
    if (fseeko(f, (off_t)h.toc_offset, SEEK_SET) != 0) return false;
#endif
    
    toc.clear();
    toc.reserve(h.file_count);
    
    for (uint32_t i = 0; i < h.file_count; ++i) {
        FileEntry fe;
        if (fread(&fe.meta, sizeof(Entry), 1, f) != 1) return false;
        
        if (fe.meta.name_len > 4096) return false;
        
        std::vector<char> name_buf(fe.meta.name_len + 1, 0);
        if (fread(name_buf.data(), 1, fe.meta.name_len, f) != fe.meta.name_len) return false;
        fe.name = std::string(name_buf.data());
        
        toc.push_back(fe);
    }
    return true;
}

bool write_toc(FILE* f, Header& h, std::vector<FileEntry>& toc) {
    fflush(f);
    
#ifdef _WIN32
    __int64 toc_pos = _ftelli64(f);
    if (toc_pos == -1) return false;
    h.toc_offset = (uint64_t)toc_pos;
#else
    off_t toc_pos = ftello(f);
    if (toc_pos == -1) return false;
    h.toc_offset = (uint64_t)toc_pos;
#endif
    
    h.file_count = static_cast<uint32_t>(toc.size());

    for (auto& fe : toc) {
        fe.meta.name_len = static_cast<uint16_t>(fe.name.length());
        if (fwrite(&fe.meta, sizeof(Entry), 1, f) != 1) return false;
        if (fwrite(fe.name.c_str(), 1, fe.meta.name_len, f) != fe.meta.name_len) return false;
    }

    if (fseek(f, 0, SEEK_SET) != 0) return false;
    if (fwrite(&h, sizeof(Header), 1, f) != 1) return false;
    
    if (fseek(f, 0, SEEK_END) != 0) return false;
    fflush(f);
    return true;
}

bool write_file_to_disk(const std::string& path, const char* data, size_t size, uint64_t timestamp) {
    try {
        fs::path p(path);
        
        if (p.has_parent_path()) {
            std::error_code ec;
            fs::create_directories(p.parent_path(), ec);
            if (ec) {
                UI::print_warning("Impossibile creare directory per: " + path);
                return false;
            }
        }
        
        FILE* out = fopen(path.c_str(), "wb");
        if (!out) {
            UI::print_warning("Impossibile aprire in scrittura: " + path);
            return false;
        }
        
        size_t bytes_written = 0;
        if (size > 0 && data != nullptr) {
            bytes_written = fwrite(data, 1, size, out);
        }
        fclose(out);
        
        if (bytes_written != size && size > 0) {
            UI::print_warning("Scrittura incompleta per: " + path);
            return false;
        }
        
        if (timestamp > 0) {
            auto sys_time = std::chrono::system_clock::from_time_t((time_t)timestamp);
            std::error_code ec;
            fs::last_write_time(p, fs::file_time_type(sys_time.time_since_epoch()), ec);
        }
        
        return true;
    } catch (const std::exception& e) {
        UI::print_warning("Eccezione in write_file_to_disk: " + std::string(e.what()));
        return false;
    }
}

bool read_bytes(FILE* f, void* buf, size_t size) {
    return fread(buf, 1, size, f) == size;
}

bool write_bytes(FILE* f, const void* buf, size_t size) {
    return fwrite(buf, 1, size, f) == size;
}

} // namespace IO
