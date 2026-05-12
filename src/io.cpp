#include "io.h"
#include "types.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <chrono>
#include <cstring>
#include <algorithm>
#include <system_error>
#include <ctime>
#include <regex>

#ifdef _WIN32
    #define NOMINMAX
    #include <windows.h>
#else
    #include <sys/time.h>
#endif

#ifdef __APPLE__
    #include <mach-o/dyld.h>
#endif

namespace fs = std::filesystem;

// BUG FIX #3: glob matching helper for Unix (not used on Windows)
#ifndef _WIN32
static bool glob_match(const std::string& name, const std::string& pattern) {
    if (pattern.empty()) return name.empty();

    // Convert glob pattern to regex
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
#endif

std::string IO::ensure_ext(const std::string& path) {
    if (path.length() < 5 || path.substr(path.length() - 5) != ".strk") {
        return path + ".strk";
    }
    return path;
}

bool IO::expand_path(const std::string& pattern, std::vector<std::string>& out) {
    // First handle directly as file/directory path
    if (fs::exists(pattern)) {
        if (fs::is_regular_file(pattern)) {
            out.push_back(pattern);
            return true;
        }
        if (fs::is_directory(pattern)) {
            // BUG FIX #8: skip_permission_denied to avoid crash
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
    // Windows: the shell does NOT expand wildcards (*, ?)
    // FindFirstFileW handles Unicode and long paths
    std::string directory = "";
    std::string filePattern = pattern;
    
    // Extract directory and file pattern
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
        // If it fails with the full path, try with the pattern only
        int wpattern_len = MultiByteToWideChar(CP_UTF8, 0, pattern.c_str(), -1, NULL, 0);
        std::wstring wpattern;
        if (wpattern_len > 1) {
            wpattern.resize(wpattern_len - 1);
            MultiByteToWideChar(CP_UTF8, 0, pattern.c_str(), -1, &wpattern[0], wpattern_len);
        }
        hFind = wpattern.empty() ? INVALID_HANDLE_VALUE : FindFirstFileW(wpattern.c_str(), &findData);
        if (hFind == INVALID_HANDLE_VALUE) return false;
        directory = "";         // Reset directory if using pattern without path
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
            // Remove leading .\ if present
            if (fullPath.substr(0, 2) == ".\\") fullPath = fullPath.substr(2);
            
            std::error_code _ec;
            if (fs::exists(fullPath, _ec)) {
                if (fs::is_regular_file(fullPath, _ec)) {
                    out.push_back(fullPath);
                }
            }
        }
    } while (FindNextFileW(hFind, &findData));
    
    FindClose(hFind);
    return !out.empty();
#else
    // Unix: the shell expands wildcards, but we still handle with glob
    if (fs::exists(pattern)) {
        if (fs::is_directory(pattern)) {
            // BUG FIX #8: circular symlink protection
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

    // BUG FIX #3: fallback glob matching on Unix if the pattern contains wildcards
    // and the shell has not expanded them (e.g. quoted pattern)
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
    
    if (IO::tarc_fseek(f, static_cast<int64_t>(h.toc_offset), SEEK_SET) != 0) return false;
    
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
    
    // SEC-005: Validazione dimensione nome (cross-platform, non legata al SO)
    if (fe.meta.name_len == 0 || fe.meta.name_len > TARC_MAX_NAME_LEN) {
        return Result<FileEntry>{TarcError::CorruptedArchive, std::nullopt};
    }

    std::vector<char> name_buf(fe.meta.name_len + 1, 0);
    if (fread(name_buf.data(), 1, fe.meta.name_len, f) != fe.meta.name_len) {
        return Result<FileEntry>{TarcError::CorruptedArchive, std::nullopt};
    }
    name_buf[fe.meta.name_len] = '\0';  // SEC-005: forza terminazione null

    // SEC-005: Verifica che il nome non contenga null bytes nel mezzo
    if (std::strlen(name_buf.data()) != fe.meta.name_len) {
        return Result<FileEntry>{TarcError::UnsafeFilename, std::nullopt};
    }

    fe.name = std::string(name_buf.data());

    // SEC-005: Validazione completa del filename
    if (!is_safe_filename(fe.name)) {
        return Result<FileEntry>{TarcError::UnsafeFilename, std::nullopt};
    }
    return Result<FileEntry>{TarcError::None, fe};
}

bool IO::write_toc(FILE* f, Header& h, std::vector<FileEntry>& toc) {
    if (fflush(f) != 0) return false;
    int64_t toc_pos = IO::tarc_ftell(f);
    if (toc_pos == -1) return false;
    
    h.toc_offset = (uint64_t)toc_pos;
    h.file_count = static_cast<uint32_t>(toc.size());

    for (auto& fe : toc) {
        if (fe.name.length() > UINT16_MAX) return false;
        fe.meta.name_len = static_cast<uint16_t>(fe.name.length());
        if (!write_entry(f, fe)) return false;
    }

    if (IO::tarc_fseek(f, 0, SEEK_SET) != 0) return false;
    if (fwrite(&h, sizeof(Header), 1, f) != 1) return false;
    
    if (IO::tarc_fseek(f, 0, SEEK_END) != 0) return false;
    if (fflush(f) != 0) return false;
    return true;
}

bool IO::write_entry(FILE* f, const FileEntry& entry) {
    if (fwrite(&entry.meta, sizeof(Entry), 1, f) != 1) return false;
    if (fwrite(entry.name.c_str(), 1, entry.meta.name_len, f) != entry.meta.name_len) return false;
    return true;
}

// ============================================================================
// SEC-001: Magic header validation
// ============================================================================
bool IO::validate_archive_header(const Header& h) {
    // Verifica magic bytes "TRC2"
    if (std::memcmp(h.magic, TARC_MAGIC, 4) != 0) {
        return false;
    }
    // SEC-003: Validazione versione
    if (h.version < TARC_VERSION_MIN || h.version > TARC_VERSION) {
        return false;
    }
    // Verifica ragionevolezza del TOC offset
    if (h.toc_offset < sizeof(Header)) {
        return false;
    }
    return true;
}

// ============================================================================
// SEC-002: Path sanitization to prevent Zip Slip (path traversal)
// ============================================================================
std::string IO::sanitize_extract_path(const std::string& raw_path) {
    // Rifiuta path vuoti
    if (raw_path.empty()) return "";

    // Rifiuta path che contengono null bytes
    if (raw_path.find('\0') != std::string::npos) return "";

    // Converti backslash in slash per normalizzazione
    std::string path = raw_path;
    std::replace(path.begin(), path.end(), '\\', '/');

    // Normalize Unix absolute paths (e.g. /BACKUP-Blustring/test/file.mdb)
    // by removing the leading slash. This happens when the archive was
    // created on Linux/macOS with absolute paths. It is not a path traversal,
    // the path simply needs to be treated as relative to output_dir.
    // NOTE: the subsequent ".." check prevents any actual traversal attempt.
    while (!path.empty() && path[0] == '/') {
        path = path.substr(1);
    }

    // Rifiuta drive letter Windows (es. C:/Windows)
#ifdef _WIN32
    if (path.size() >= 2 && path[1] == ':') return "";
#endif

    // Rifiuta path che contengono ".." (path traversal)
    // Normalizza il path prima di verificare
    std::vector<std::string> parts;
    std::string current;
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '/') {
            if (!current.empty()) {
                parts.push_back(current);
                current.clear();
            }
        } else {
            current += path[i];
        }
    }
    if (!current.empty()) {
        parts.push_back(current);
    }

    // Risolvi . e .. nel path
    // Block ONLY the pure ".." component (directory traversal).
    // Legitimate filenames containing ".." like "AINELFS.R.L..mdb"
    // or "file..old.txt" are accepted without issues.
    std::vector<std::string> resolved;
    for (const auto& part : parts) {
        if (part == ".") {
            continue;
        } else if (part == "..") {
            // Path traversal: pure ".." component (parent directory)
            return "";
        } else {
            resolved.push_back(part);
        }
    }

    // Ricostruisci il path normalizzato
    std::string result;
    for (size_t i = 0; i < resolved.size(); ++i) {
        if (i > 0) result += '/';
        result += resolved[i];
    }

    return result;
}

// ============================================================================
// SEC-005: Filename validation
// ============================================================================
bool IO::is_safe_filename(const std::string& name) {
    if (name.empty()) return false;
    if (name.size() > TARC_MAX_NAME_LEN) return false;

    // Rifiuta null bytes
    if (name.find('\0') != std::string::npos) return false;

    // Rifiuta caratteri di controllo (0x00-0x1F tranne tab)
    for (char c : name) {
        if (static_cast<unsigned char>(c) < 0x20 && c != '\t') return false;
    }

    // NOTE: the ".." (path traversal) check is delegated to sanitize_extract_path()
    // which normalizes the path and rejects only actual ".." components.
    // Here we do not reject ".." in the name because legitimate files like
    // "my_file_v2..bak.txt" or "file..old" must be accepted.

    return true;
}

// ============================================================================
// SEC-007: File existence check
// ============================================================================
bool IO::file_exists(const std::string& path) {
    std::error_code ec;
    return fs::exists(path, ec) && !ec;
}

// ============================================================================
// write_file_to_disk updated with overwrite protection
// ============================================================================
bool IO::write_file_to_disk(const std::string& path, const char* data, size_t size,
                               uint64_t timestamp, bool overwrite) {
    try {
        // SEC-002: Sanitizza il path prima di scrivere
        std::string safe_path = sanitize_extract_path(path);
        if (safe_path.empty()) {
            return false;
        }

        fs::path p(safe_path);

        // SEC-007: Check if the file already exists (overwrite protection)
        if (!overwrite && fs::exists(p)) {
            return false;
        }

        if (p.has_parent_path()) {
            std::error_code ec;
            fs::create_directories(p.parent_path(), ec);
            if (ec) return false;
        }

        std::ofstream out(safe_path, std::ios::binary);
        if (!out) return false;

        if (size > 0 && data != nullptr) {
            out.write(data, size);
        }
        out.close();

        if (!out.good()) return false;

        try {
#ifdef _WIN32
            auto file_time = fs::file_time_type(std::chrono::seconds(timestamp));
            fs::last_write_time(safe_path, file_time);
#else
            struct timeval tv[2];
            tv[0].tv_sec = 0;
            tv[0].tv_usec = 0;
            tv[1].tv_sec = static_cast<time_t>(timestamp);
            tv[1].tv_usec = 0;
            utimes(safe_path.c_str(), tv);
#endif
        } catch (...) {
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

// ============================================================================
// Path of the current executable (cross-platform)
// ============================================================================
std::string IO::get_self_path() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return "";
    int narrow_len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    if (narrow_len <= 0) return "";
    std::string result(narrow_len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, &result[0], narrow_len, nullptr, nullptr);
    return result;
#elif defined(__APPLE__)
    // macOS: uses _NSGetExecutablePath (mach-o/dyld.h)
    // /proc/self/exe does NOT exist on macOS!
    char buf[4096];
    uint32_t buf_size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &buf_size) != 0) return "";
    // Resolve relative path to absolute
    char resolved[4096];
    if (realpath(buf, resolved) != nullptr) {
        return std::string(resolved);
    }
    return std::string(buf);
#else
    // Linux / BSD with /proc filesystem
    std::string result;
    result.resize(4096);
    ssize_t len = readlink("/proc/self/exe", &result[0], result.size());
    if (len <= 0) return "";
    if (static_cast<size_t>(len) >= result.size()) {
        // Path troppo lungo per il buffer statico: ridimensiona e riprova
        result.resize(len + 1);
        len = readlink("/proc/self/exe", &result[0], result.size());
        if (len <= 0) return "";
    }
    result.resize(static_cast<size_t>(len));
    return result;
#endif
}