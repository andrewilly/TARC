#include "io.h"
#include "types.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <chrono>
#include <cstring>
#include <system_error>
#include <ctime>
#include <regex>

#ifdef _WIN32
    #include <windows.h>
#endif

namespace fs = std::filesystem;

// BUG FIX #3: helper glob matching per Unix
static bool glob_match(const std::string& name, const std::string& pattern) {
    if (pattern.empty()) return name.empty();

    // Converti pattern glob in regex
    std::string regex_str;
    for (size_t i = 0; i < pattern.size(); ++i) {
        char c = pattern[i];
        if (c == '*') {
            regex_str += ".*";
        } else if (c == '?') {
            regex_str += ".";
        } else if (c == '.' || c == '(' || c == ')' || c == '[' || c == ']' ||
                   c == '{' || c == '}' || c == '+' || c == '^' || c == '$' ||
                   c == '|' || c == '\\') {
            regex_str += '\\';
            regex_str += c;
        } else {
            regex_str += c;
        }
    }

    try {
        std::regex re("^" + regex_str + "$", std::regex::icase);
        return std::regex_match(name, re);
    } catch (...) {
        return name == pattern;
    }
}

std::string IO::ensure_ext(const std::string& path) {
    if (path.length() < 5 || path.substr(path.length() - 5) != ".strk") {
        return path + ".strk";
    }
    return path;
}

bool IO::expand_path(const std::string& pattern, std::vector<std::string>& out) {
    // Prima gestisci direttamente come percorso file/directory
    if (fs::exists(pattern)) {
        if (fs::is_regular_file(pattern)) {
            out.push_back(pattern);
            return true;
        }
        if (fs::is_directory(pattern)) {
            // BUG FIX #8: skip_permission_denied per evitare crash
            for (auto& p : fs::recursive_directory_iterator(
                    pattern, fs::directory_options::skip_permission_denied)) {
                if (p.is_regular_file()) {
                    out.push_back(p.path().string());
                }
            }
            return !out.empty();
        }
    }
    
    // Se non esiste direttamente, prova come pattern con wildcard
#ifdef _WIN32
    // Windows: la shell NON espande le wildcard (*, ?)
    // FindFirstFileW gestisce Unicode e long path
    std::string directory = "";
    std::string filePattern = pattern;
    
    // Estrai directory e pattern file
    size_t last_slash = pattern.find_last_of("\\/");
    if (last_slash != std::string::npos) {
        directory = pattern.substr(0, last_slash + 1);
        filePattern = pattern.substr(last_slash + 1);
    } else {
        directory = "";
    }
    
    if (filePattern.find('*') == std::string::npos && filePattern.find('?') == std::string::npos) {
        return false;
    }
    std::string searchPath = directory + filePattern;
    int wsearch_len = MultiByteToWideChar(CP_UTF8, 0, searchPath.c_str(), -1, NULL, 0);
    std::wstring wsearchPath;
    if (wsearch_len > 1) {
        wsearchPath.resize(wsearch_len - 1);
        MultiByteToWideChar(CP_UTF8, 0, searchPath.c_str(), -1, &wsearchPath[0], wsearch_len);
    }
    WIN32_FIND_DATAW findData;
    HANDLE hFind = wsearchPath.empty() ? INVALID_HANDLE_VALUE : FindFirstFileW(wsearchPath.c_str(), &findData);
    
    if (hFind == INVALID_HANDLE_VALUE) {
        // Se fallisce con tutto il percorso, prova solo con il pattern
        int wpattern_len = MultiByteToWideChar(CP_UTF8, 0, pattern.c_str(), -1, NULL, 0);
        std::wstring wpattern;
        if (wpattern_len > 1) {
            wpattern.resize(wpattern_len - 1);
            MultiByteToWideChar(CP_UTF8, 0, pattern.c_str(), -1, &wpattern[0], wpattern_len);
        }
        hFind = wpattern.empty() ? INVALID_HANDLE_VALUE : FindFirstFileW(wpattern.c_str(), &findData);
        if (hFind == INVALID_HANDLE_VALUE) return false;
        directory = ""; // Reset directory se usiamo pattern senza percorso
    }
    
    do {
        std::wstring wname(findData.cFileName);
        int narrow_len = WideCharToMultiByte(CP_UTF8, 0, wname.c_str(), -1, NULL, 0, NULL, NULL);
        std::string foundName;
        if (narrow_len > 0) {
            foundName.resize(narrow_len - 1);
            WideCharToMultiByte(CP_UTF8, 0, wname.c_str(), -1, &foundName[0], narrow_len, NULL, NULL);
        }
        if (foundName != "." && foundName != "..") {
            std::string fullPath = directory + foundName;
            // Rimuovi .\ iniziale se presente
            if (fullPath.substr(0, 2) == ".\\") fullPath = fullPath.substr(2);
            
            if (fs::exists(fullPath)) {
                if (fs::is_regular_file(fullPath)) {
                    out.push_back(fullPath);
                }
            }
        }
    } while (FindNextFileW(hFind, &findData));
    
    FindClose(hFind);
    return !out.empty();
#else
    // Unix: la shell espande le wildcard, ma gestiamo comunque con glob
    if (fs::exists(pattern)) {
        if (fs::is_directory(pattern)) {
            // BUG FIX #8: protezione symlink circolari
            for (auto& p : fs::recursive_directory_iterator(
                    pattern, fs::directory_options::skip_permission_denied)) {
                if (p.is_regular_file() && !p.is_symlink()) {
                    out.push_back(p.path().string());
                }
            }
        } else {
            out.push_back(pattern);
        }
        return true;
    }

    // BUG FIX #3: fallback glob matching su Unix se il pattern contiene wildcard
    // e la shell non le ha espanse (es. pattern quotato)
    bool has_glob = (pattern.find('*') != std::string::npos ||
                     pattern.find('?') != std::string::npos);
    if (has_glob) {
        fs::path parent_dir = fs::path(pattern).parent_path();
        std::string file_pat = fs::path(pattern).filename().string();

        if (parent_dir.empty() || !fs::exists(parent_dir)) {
            parent_dir = fs::current_path();
        }

        if (fs::is_directory(parent_dir)) {
            for (auto& p : fs::directory_iterator(parent_dir)) {
                std::string name = p.path().filename().string();
                bool match = false;

                // Match semplice: '*' = qualsiasi cosa, '?' = un char
                if (file_pat.find('*') != std::string::npos ||
                    file_pat.find('?') != std::string::npos) {
                    // Converti il pattern glob in regex semplificato
                    match = glob_match(name, file_pat);
                } else {
                    match = (name == file_pat);
                }

                if (match && p.is_regular_file()) {
                    out.push_back(p.path().string());
                }
            }
            return !out.empty();
        }
    }
    return false;
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