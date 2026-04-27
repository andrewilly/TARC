#include "io.h"
#include "types.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <chrono>
#include <cstring>
#include <system_error>
#include <ctime>

#ifdef _WIN32
    #include <windows.h>
#endif

namespace fs = std::filesystem;

std::string IO::ensure_ext(const std::string& path) {
    if (path.length() < 5 || path.substr(path.length() - 5) != ".strk") {
        return path + ".strk";
    }
    return path;
}

bool IO::expand_path(const std::string& pattern, std::vector<std::string>& out) {
    if (pattern.empty()) return false;
    
    std::string path_to_check = pattern;
    std::string search_pattern = pattern;
    
    size_t last_slash = pattern.find_last_of("\\/");
    std::string directory = "";
    if (last_slash != std::string::npos) {
        directory = pattern.substr(0, last_slash + 1);
    }
    
    if (fs::is_directory(pattern)) {
        for (auto& p : fs::recursive_directory_iterator(pattern)) {
            if (p.is_regular_file()) {
                out.push_back(p.path().string());
            }
        }
        return true;
    }
    
    if (directory.empty()) {
        directory = ".";
        search_pattern = pattern;
    }

#ifdef _WIN32
    WIN32_FIND_DATAA findData;
    HANDLE hFind = FindFirstFileA(search_pattern.c_str(), &findData);
    if (hFind == INVALID_HANDLE_VALUE) return false;

    do {
        std::string foundName(findData.cFileName);
        if (foundName != "." && foundName != "..") {
            std::string fullPath = directory + foundName;
            if (fs::exists(fullPath)) {
                if (fs::is_regular_file(fullPath)) out.push_back(fullPath);
                else if (fs::is_directory(fullPath)) {
                    for (auto& p : fs::recursive_directory_iterator(fullPath))
                        if (p.is_regular_file()) out.push_back(p.path().string());
                }
            }
        }
    } while (FindNextFileA(hFind, &findData));
    FindClose(hFind);
    return true;
#else
    if (fs::exists(search_pattern)) {
        if (fs::is_regular_file(search_pattern)) {
            out.push_back(search_pattern);
        }
    }
    return !out.empty();
#endif
}

bool IO::read_toc(FILE* f, Header& h, std::vector<FileEntry>& toc) {
    if (h.toc_offset == 0) return false;
    
    if (fseek(f, (long)h.toc_offset, SEEK_SET) != 0) return false;
    
    toc.clear();
    toc.reserve(h.file_count);
    
    for (uint32_t i = 0; i < h.file_count; ++i) {
        auto entry = IO::read_entry(f);
        if (!entry.has_value()) return false;
        toc.push_back(*entry);
    }
    return true;
}

Result<FileEntry> IO::read_entry(FILE* f) {
    FileEntry fe;
    if (fread(&fe.meta, sizeof(Entry), 1, f) != 1) {
        return Result<FileEntry>{TarcError::CorruptedArchive, std::nullopt};
    }
    
    if (fe.meta.name_len > 4096) {
        return Result<FileEntry>{TarcError::CorruptedArchive, std::nullopt};
    }
    
    std::vector<char> name_buf(fe.meta.name_len + 1, 0);
    if (fread(name_buf.data(), 1, fe.meta.name_len, f) != fe.meta.name_len) {
        return Result<FileEntry>{TarcError::CorruptedArchive, std::nullopt};
    }
    fe.name = std::string(name_buf.data());
    return Result<FileEntry>{TarcError::None, fe};
}

bool IO::write_toc(FILE* f, Header& h, std::vector<FileEntry>& toc) {
    fflush(f);
    long toc_pos = ftell(f);
    if (toc_pos == -1) return false;
    
    h.toc_offset = (uint64_t)toc_pos;
    h.file_count = static_cast<uint32_t>(toc.size());

    for (auto& fe : toc) {
        fe.meta.name_len = static_cast<uint16_t>(fe.name.length());
        if (!write_entry(f, fe)) return false;
    }

    if (fseek(f, 0, SEEK_SET) != 0) return false;
    if (fwrite(&h, sizeof(Header), 1, f) != 1) return false;
    
    if (fseek(f, 0, SEEK_END) != 0) return false;
    fflush(f);
    return true;
}

bool IO::write_entry(FILE* f, const FileEntry& entry) {
    if (fwrite(&entry.meta, sizeof(Entry), 1, f) != 1) return false;
    if (fwrite(entry.name.c_str(), 1, entry.meta.name_len, f) != entry.meta.name_len) return false;
    return true;
}

bool IO::write_file_to_disk(const std::string& path, const char* data, size_t size, uint64_t timestamp) {
    try {
        fs::path p(path);
        
        if (p.has_parent_path()) {
            std::error_code ec;
            fs::create_directories(p.parent_path(), ec);
            if (ec) return false;
        }

        std::ofstream out(path, std::ios::binary);
        if (!out) return false;

        if (size > 0 && data != nullptr) {
            out.write(data, size);
        }
        out.close();

        if (out.good()) {
            try {
#ifdef _WIN32
                auto file_time = fs::file_time_type(std::chrono::seconds(timestamp));
                fs::last_write_time(p, file_time);
#else
                (void)timestamp;
#endif
            } catch (...) {
            }
        }
        
        return true;
    } catch (...) {
        return false;
    }
}

bool IO::read_bytes(FILE* f, void* buf, size_t size) {
    return fread(buf, 1, size, f) == size;
}

bool IO::write_bytes(FILE* f, const void* buf, size_t size) {
    return fwrite(buf, 1, size, f) == size;
}