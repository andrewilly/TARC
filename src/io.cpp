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
#include <algorithm>
#include <set>

#ifdef _WIN32
    #include <windows.h>
#endif

namespace fs = std::filesystem;

// BUG FIX #3: helper glob matching per Unix
static bool glob_match(const std::string& name, const std::string& pattern) {
    if (pattern.empty()) return name.empty();

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
    std::string directory = "";
    std::string filePattern = pattern;

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
        int wpattern_len = MultiByteToWideChar(CP_UTF8, 0, pattern.c_str(), -1, NULL, 0);
        std::wstring wpattern;
        if (wpattern_len > 1) {
            wpattern.resize(wpattern_len - 1);
            MultiByteToWideChar(CP_UTF8, 0, pattern.c_str(), -1, &wpattern[0], wpattern_len);
        }
        hFind = wpattern.empty() ? INVALID_HANDLE_VALUE : FindFirstFileW(wpattern.c_str(), &findData);
        if (hFind == INVALID_HANDLE_VALUE) return false;
        directory = "";
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
    if (fs::exists(pattern)) {
        if (fs::is_directory(pattern)) {
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

                if (file_pat.find('*') != std::string::npos ||
                    file_pat.find('?') != std::string::npos) {
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

    if (fe.meta.name_len > TARC_MAX_NAME_LEN || fe.meta.name_len == 0) {
        return Result<FileEntry>{TarcError::CorruptedArchive, std::nullopt};
    }

    std::vector<char> name_buf(fe.meta.name_len + 1, 0);
    if (fread(name_buf.data(), 1, fe.meta.name_len, f) != fe.meta.name_len) {
        return Result<FileEntry>{TarcError::CorruptedArchive, std::nullopt};
    }

    if (std::strlen(name_buf.data()) != fe.meta.name_len) {
        return Result<FileEntry>{TarcError::CorruptedArchive, std::nullopt};
    }

    fe.name = std::string(name_buf.data());
    return Result<FileEntry>{TarcError::None, fe};
}

bool IO::write_toc(FILE* f, Header& h, std::vector<FileEntry>& toc) {
    fflush(f);
    int64_t toc_pos = IO::tarc_ftell(f);
    if (toc_pos == -1) return false;

    h.toc_offset = (uint64_t)toc_pos;
    h.file_count = static_cast<uint32_t>(toc.size());

    for (auto& fe : toc) {
        fe.meta.name_len = static_cast<uint16_t>(fe.name.length());
        if (!write_entry(f, fe)) return false;
    }

    if (IO::tarc_fseek(f, 0, SEEK_SET) != 0) return false;
    if (fwrite(&h, sizeof(Header), 1, f) != 1) return false;

    if (IO::tarc_fseek(f, 0, SEEK_END) != 0) return false;
    fflush(f);
    return true;
}

bool IO::write_entry(FILE* f, const FileEntry& entry) {
    if (fwrite(&entry.meta, sizeof(Entry), 1, f) != 1) return false;
    if (fwrite(entry.name.c_str(), 1, entry.meta.name_len, f) != entry.meta.name_len) return false;
    return true;
}

// ARCH-008: output_dir support — prepend custom output directory to extraction path
bool IO::write_file_to_disk(const std::string& path, const char* data, size_t size,
                            uint64_t timestamp, bool overwrite,
                            const std::string& output_dir) {
    try {
        // Build final path: output_dir + path
        std::string final_path;
        if (!output_dir.empty()) {
            // Ensure trailing slash on output_dir
            if (output_dir.back() == '/' || output_dir.back() == '\\') {
                final_path = output_dir + path;
            } else {
                final_path = output_dir + "/" + path;
            }
        } else {
            final_path = path;
        }

        fs::path p(final_path);

        if (p.has_parent_path()) {
            std::error_code ec;
            fs::create_directories(p.parent_path(), ec);
            if (ec) return false;
        }

        if (!overwrite && fs::exists(p)) {
            return false;
        }

        std::ofstream out(final_path, std::ios::binary);
        if (!out) return false;

        if (size > 0 && data != nullptr) {
            out.write(data, size);
        }
        out.close();

        if (out.good()) {
            try {
                // ARCH-007: timestamp restoration on all platforms (not just Windows)
                auto file_time = fs::file_time_type(std::chrono::seconds(timestamp));
                fs::last_write_time(p, file_time);
            } catch (...) {
                // Silently ignore timestamp restoration failure
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

// ============================================================
// Security function implementations
// ============================================================

bool IO::validate_archive_header(const Header& h) {
    if (std::memcmp(h.magic, TARC_MAGIC, 4) != 0) {
        return false;
    }

    if (h.version < TARC_VERSION_MIN || h.version > TARC_VERSION) {
        return false;
    }

    if (h.toc_offset == 0 && h.file_count > 0) {
        return false;
    }

    return true;
}

std::string IO::sanitize_extract_path(const std::string& entry_name) {
    if (entry_name.empty()) return "";

    if (entry_name[0] == '/' || entry_name[0] == '\\') {
        return "";
    }

#ifdef _WIN32
    if (entry_name.size() >= 2 && entry_name[1] == ':' &&
        ((entry_name[0] >= 'A' && entry_name[0] <= 'Z') ||
         (entry_name[0] >= 'a' && entry_name[0] <= 'z'))) {
        return "";
    }
#endif

    if (entry_name.find('\0') != std::string::npos) {
        return "";
    }

    std::string result = entry_name;
    std::replace(result.begin(), result.end(), '\\', '/');

    size_t pos = 0;
    while (pos < result.size()) {
        while (pos < result.size() && result[pos] == '/') pos++;

        size_t end = result.find('/', pos);
        if (end == std::string::npos) end = result.size();

        std::string component = result.substr(pos, end - pos);

        if (component == "..") {
            return "";
        }

        pos = end;
    }

    return entry_name;
}

bool IO::is_safe_filename(const std::string& name) {
    if (name.empty()) return false;

    for (unsigned char c : name) {
        if (c < 0x20 || c == 0x7F) {
            return false;
        }
    }

    return true;
}

bool IO::file_exists(const std::string& path) {
    try {
        return fs::exists(path) && fs::is_regular_file(path);
    } catch (...) {
        return false;
    }
}
