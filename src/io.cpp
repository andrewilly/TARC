#include "io.h"
#include "types.h"
#include "ui.h"
#include <filesystem>
#include <fstream>
#include <chrono>
#include <cstring>
#include <algorithm>

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

std::string normalize_path(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}

void resolve_wildcards(const std::string& pattern, std::vector<std::string>& out) {
#ifdef _WIN32
    WIN32_FIND_DATAA findData;
    HANDLE hFind = FindFirstFileA(pattern.c_str(), &findData);
    if (hFind == INVALID_HANDLE_VALUE) return;
    
    std::string directory = "";
    size_t last_slash = pattern.find_last_of("\\/");
    if (last_slash != std::string::npos) directory = pattern.substr(0, last_slash + 1);
    
    do {
        std::string foundName(findData.cFileName);
        if (foundName != "." && foundName != "..") {
            std::string fullPath = directory + foundName;
            if (fs::exists(fullPath)) {
                if (fs::is_regular_file(fullPath)) {
                    out.push_back(normalize_path(fullPath));
                } else if (fs::is_directory(fullPath)) {
                    for (auto& p : fs::recursive_directory_iterator(fullPath)) {
                        if (p.is_regular_file()) {
                            out.push_back(normalize_path(p.path().string()));
                        }
                    }
                }
            }
        }
    } while (FindNextFileA(hFind, &findData));
    FindClose(hFind);
#else
    if (fs::exists(pattern)) {
        if (fs::is_directory(pattern)) {
            for (auto& p : fs::recursive_directory_iterator(pattern)) {
                if (p.is_regular_file()) {
                    out.push_back(normalize_path(p.path().string()));
                }
            }
        } else {
            out.push_back(normalize_path(pattern));
        }
    }
#endif
}

bool read_toc(FILE* f, Header& h, std::vector<FileEntry>& toc) {
    if (h.toc_offset == 0) return false;
    
#ifdef _WIN32
    if (_fseeki64(f, static_cast<__int64>(h.toc_offset), SEEK_SET) != 0) return false;
#else
    if (fseeko(f, static_cast<off_t>(h.toc_offset), SEEK_SET) != 0) return false;
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
        
        toc.push_back(std::move(fe));
    }
    return true;
}

bool write_toc(FILE* f, Header& h, std::vector<FileEntry>& toc) {
    fflush(f);
    
#ifdef _WIN32
    __int64 toc_pos = _ftelli64(f);
    if (toc_pos == -1) return false;
    h.toc_offset = static_cast<uint64_t>(toc_pos);
#else
    off_t toc_pos = ftello(f);
    if (toc_pos == -1) return false;
    h.toc_offset = static_cast<uint64_t>(toc_pos);
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
            if (ec) return false;
        }
        
        FILE* out = fopen(path.c_str(), "wb");
        if (!out) return false;
        
        if (size > 0 && data != nullptr) {
            fwrite(data, 1, size, out);
        }
        fclose(out);
        
        if (timestamp > 0) {
            auto sys_time = std::chrono::system_clock::from_time_t(static_cast<time_t>(timestamp));
            std::error_code ec;
            fs::last_write_time(p, fs::file_time_type(sys_time.time_since_epoch()), ec);
        }
        
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

bool match_pattern(const std::string& full_path, const std::string& pattern) {
    if (pattern.empty()) return true;
    
    std::string target = full_path;
    
    // Se pattern non contiene slash, usa solo filename
    if (pattern.find('/') == std::string::npos && pattern.find('\\') == std::string::npos) {
        target = fs::path(full_path).filename().string();
    }
    
    size_t star_pos = pattern.find('*');
    if (star_pos == std::string::npos) {
        return target.find(pattern) != std::string::npos;
    }
    
    std::string prefix = pattern.substr(0, star_pos);
    std::string suffix = pattern.substr(star_pos + 1);
    
    if (!prefix.empty() && target.find(prefix) != 0) return false;
    if (!suffix.empty()) {
        if (suffix.length() > target.length()) return false;
        if (target.compare(target.length() - suffix.length(), suffix.length(), suffix) != 0) return false;
    }
    
    return true;
}

} // namespace IO
