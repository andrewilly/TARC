#include "io.h"
#include "types.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <chrono>
#include <cstring>
#include <system_error>
#include <ctime>
#include <random>
#include <algorithm>

static std::string normalize_path_str(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}

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

#ifdef _WIN32
FILE* IO::u8fopen(const std::string& path, const std::string& mode) {
    return _wfopen(fs::u8path(path).wstring().c_str(), fs::u8path(mode).wstring().c_str());
}
#else
FILE* IO::u8fopen(const std::string& path, const std::string& mode) {
    return fopen(path.c_str(), mode.c_str());
}
#endif

bool IO::seek64(FILE* f, int64_t offset, int origin) {
    #ifdef _WIN32
    return _fseeki64(f, offset, origin) == 0;
    #else
    return fseek(f, offset, origin) == 0;
    #endif
}

bool IO::validate_header(const Header& h) {
    if (std::memcmp(h.magic, TARC_MAGIC, 4) != 0 &&
        std::memcmp(h.magic, "TRC1", 4) != 0 &&
        std::memcmp(h.magic, "TRC2", 4) != 0) {
        return false;
    }
    return true;
}

std::string IO::make_temp_path(const std::string& path) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);
    
    std::string ext = fs::path(path).extension().string();
    std::string dir = fs::path(path).parent_path().string();
    std::string stem = fs::path(path).stem().string();
    
    std::string temp;
    do {
        std::string suffix;
        for (int i = 0; i < 8; ++i) {
            suffix += "0123456789abcdef"[dis(gen)];
        }
        temp = dir + "/" + stem + ".tmp_" + suffix + ext;
    } while (fs::exists(temp));
    
    return temp;
}

bool IO::safe_remove(const std::string& path) {
    try {
        return fs::remove(path);
    } catch (...) {
        return false;
    }
}

bool IO::atomic_rename(const std::string& from, const std::string& to) {
    #ifdef _WIN32
    return MoveFileExA(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    #else
    return rename(from.c_str(), to.c_str()) == 0;
    #endif
}

std::string IO::sanitize_path(const std::string& path) {
    std::string result = path;
    
    // Rimuovi path components pericolosi
    std::vector<std::string> dangerous = {"..", "~", "$", "`", "|"};
    for (const auto& d : dangerous) {
        if (result.find(d) != std::string::npos) {
            return "";
        }
    }
    
    // Unix: non permettere path assoluti
    if (!result.empty() && result[0] == '/') {
        return "";
    }
    
    // Windows: non permettere drive letters esterne a C:
    #ifdef _WIN32
    if (result.length() >= 2 && result[1] == ':') {
        char drive = std::toupper(result[0]);
        if (drive < 'A' || drive > 'Z') return "";
        if (drive > 'C') return "";
    }
    #endif
    
    return result;
}

static std::string normalize_path_str(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}

bool IO::expand_path(const std::string& pattern, std::vector<std::string>& out) {
    if (pattern.empty()) return false;
    out.clear();

    try {
        fs::path p = fs::u8path(pattern);

        // Directory: expand recursively
        if (fs::exists(p) && fs::is_directory(p)) {
            for (auto& entry : fs::recursive_directory_iterator(p)) {
                if (entry.is_regular_file()) {
                    out.push_back(normalize_path_str(entry.path().string()));
                }
            }
            return !out.empty();
        }

        // Single existing file
        if (fs::exists(p) && fs::is_regular_file(p)) {
            out.push_back(normalize_path_str(p.string()));
            return true;
        }

        // Wildcard support
        std::string dir = ".";
        std::string file_pat = p.filename().string();

        if (p.has_parent_path()) {
            dir = p.parent_path().string();
        }

        if (file_pat.find('*') != std::string::npos || file_pat.find('?') != std::string::npos) {
            std::error_code ec;
            if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) return false;

            for (auto& entry : fs::directory_iterator(fs::u8path(dir), ec)) {
                std::string name = entry.path().filename().string();
                if (name == "." || name == "..") continue;

                if (entry.is_regular_file(ec)) {
                    bool match = true;
                    if (file_pat == "*" || file_pat == "*.*") {
                        match = true;
                    } else if (file_pat.find('*') != std::string::npos) {
                        std::string prefix = file_pat.substr(0, file_pat.find('*'));
                        match = name.find(prefix) == 0;
                    } else {
                        match = (name == file_pat);
                    }

                    if (match) {
                        out.push_back(normalize_path_str(entry.path().string()));
                    }
                }
            }
            return !out.empty();
        }

        return false;
    } catch (...) {
        return false;
    }
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