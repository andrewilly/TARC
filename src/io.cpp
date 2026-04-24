#include "io.h"
#include "types.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <chrono>
#include <cstring>
#include <random>
#include <algorithm>
#include <sstream>
#include <unistd.h>  // getpid(), isatty()

#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif

namespace fs = std::filesystem;

namespace IO {

// ═══════════════════════════════════════════════════════════════════════════════
// UNICODE-AWARE FOPEN
// Su Windows, converte il percorso UTF-8 in wide string e usa _wfopen().
// Su POSIX, UTF-8 e' nativo, quindi fopen() diretto.
// ═══════════════════════════════════════════════════════════════════════════════

FILE* u8fopen(const std::string& utf8_path, const char* mode) {
#ifdef _WIN32
    auto p = fs::u8path(utf8_path);
    std::wstring wmode;
    for (const char* c = mode; *c; ++c) {
        wmode += static_cast<wchar_t>(*c);
    }
    return _wfopen(p.c_str(), wmode.c_str());
#else
    return fopen(utf8_path.c_str(), mode);
#endif
}

// ═══════════════════════════════════════════════════════════════════════════════
// SICUREZZA PERCORSI — Sanitizzazione e validazione
// ═══════════════════════════════════════════════════════════════════════════════

std::string sanitize_path(const std::string& path) {
    if (path.empty()) return "";

    // Rifiuta percorsi assoluti
#ifdef _WIN32
    if (path.size() >= 2 && path[1] == ':') return "";
    if (path.size() >= 2 && path[0] == '\\' && path[1] == '\\') return "";
#endif
    if (!path.empty() && path[0] == '/') return "";

    // Normalizza i separatori a /
    std::string normalized = path;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');

    // Rimuovi doppi slash
    std::string::iterator new_end = std::unique(normalized.begin(), normalized.end(),
        [](char a, char b) { return a == '/' && b == '/'; });
    normalized.erase(new_end, normalized.end());

    // Split in componenti e ricostruisci senza ".."
    std::vector<std::string> parts;
    std::string part;
    std::istringstream iss(normalized);
    while (std::getline(iss, part, '/')) {
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            if (parts.empty()) return "";
            parts.pop_back();
        } else {
            // Rifiuta caratteri di controllo e null byte
            for (char c : part) {
                if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) return "";
            }
            parts.push_back(part);
        }
    }

    if (parts.empty()) return "";

    std::string safe_path;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) safe_path += '/';
        safe_path += parts[i];
    }
    return safe_path;
}

// ═══════════════════════════════════════════════════════════════════════════════
// VALIDAZIONE HEADER — Protezione da archivi malevoli
// ═══════════════════════════════════════════════════════════════════════════════

bool validate_header(const Header& h) {
    if (memcmp(h.magic, TARC_MAGIC, 4) != 0) return false;
    if (h.version < 200 || h.version > TARC_VERSION) return false;
    if (h.toc_offset > 0 && h.toc_offset < sizeof(Header)) return false;
    // Protezione OOM: limita il numero di file nell'archivio
    if (h.file_count > MAX_FILE_COUNT) return false;
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// VALIDAZIONE DIRECTORY OUTPUT
// ═══════════════════════════════════════════════════════════════════════════════

bool validate_output_dir(const std::string& dir) {
    if (dir.empty()) return true;

    // Normalizza separatori
    std::string normalized = dir;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');

    // Rifiuta componenti ".."
    std::istringstream iss(normalized);
    std::string part;
    while (std::getline(iss, part, '/')) {
        if (part == "..") return false;
    }

    // Rifiuta caratteri di controllo
    for (char c : dir) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) return false;
    }

    return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// SCRITTURE ATOMICHE — File temporanei con rename
// ═══════════════════════════════════════════════════════════════════════════════

std::string make_temp_path(const std::string& target_path) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);
    uint32_t r = dist(gen);

    // Include PID per ridurre rischio collisione tra processi
    auto pid = static_cast<uint32_t>(getpid());

    char hex[17];
    snprintf(hex, sizeof(hex), "%08x%08x", pid, r);

    return target_path + ".tmp" + hex;
}

bool atomic_rename(const std::string& from, const std::string& to) {
#ifdef _WIN32
    auto wfrom = fs::u8path(from).wstring();
    auto wto   = fs::u8path(to).wstring();
    return MoveFileExW(wfrom.c_str(), wto.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return rename(from.c_str(), to.c_str()) == 0;
#endif
}

bool safe_remove(const std::string& path) {
    std::error_code ec;
    fs::remove(fs::u8path(path), ec);
    return !ec;
}

// ═══════════════════════════════════════════════════════════════════════════════
// FUNZIONI I/O
// ═══════════════════════════════════════════════════════════════════════════════

std::string ensure_ext(const std::string& path) {
    if (path.length() < 5 || path.substr(path.length() - 5) != ".strk") {
        return path + ".strk";
    }
    return path;
}

// Espansione percorsi Unicode-aware (cross-platform)
void expand_path(const std::string& pattern, std::vector<std::string>& out) {
    auto p = fs::u8path(pattern);
    std::error_code ec;
    if (fs::exists(p, ec)) {
        if (fs::is_directory(p)) {
            for (auto& entry : fs::recursive_directory_iterator(p, ec))
                if (entry.is_regular_file()) out.push_back(entry.path().string());
        } else {
            out.push_back(p.string());
        }
    }
}

bool read_toc(FILE* f, Header& h, std::vector<FileEntry>& toc) {
    if (h.toc_offset == 0) return false;
    if (!seek64(f, static_cast<int64_t>(h.toc_offset), SEEK_SET)) return false;

    toc.clear();
    toc.reserve(h.file_count);

    for (uint32_t i = 0; i < h.file_count; ++i) {
        FileEntry fe;
        if (fread(&fe.meta, sizeof(Entry), 1, f) != 1) return false;
        if (fe.meta.name_len > MAX_NAME_LEN) return false;

        std::vector<char> name_buf(fe.meta.name_len + 1, 0);
        if (fread(name_buf.data(), 1, fe.meta.name_len, f) != fe.meta.name_len) return false;
        fe.name = std::string(name_buf.data());

        toc.push_back(fe);
    }
    return true;
}

bool write_toc(FILE* f, Header& h, std::vector<FileEntry>& toc) {
    fflush(f);
    int64_t toc_pos = tell64(f);
    if (toc_pos < 0) return false;

    h.toc_offset = static_cast<uint64_t>(toc_pos);
    h.file_count = static_cast<uint32_t>(toc.size());

    for (auto& fe : toc) {
        fe.meta.name_len = static_cast<uint16_t>(
            (std::min)(static_cast<size_t>(fe.name.length()), static_cast<size_t>(MAX_NAME_LEN)));
        if (fwrite(&fe.meta, sizeof(Entry), 1, f) != 1) return false;
        if (fwrite(fe.name.c_str(), 1, fe.meta.name_len, f) != fe.meta.name_len) return false;
    }

    if (!seek64(f, 0, SEEK_SET)) return false;
    if (fwrite(&h, sizeof(Header), 1, f) != 1) return false;

    if (!seek64(f, 0, SEEK_END)) return false;
    fflush(f);
    return true;
}

bool write_file_to_disk(const std::string& path, const char* data, size_t size, uint64_t timestamp) {
    try {
        fs::path p = fs::u8path(path);

        if (p.has_parent_path()) {
            std::error_code ec;
            fs::create_directories(p.parent_path(), ec);
            if (ec) return false;

            // Protezione symlink: verifica che le directory intermedie
            // non siano link simbolici (previene attacchi tramite symlink)
            if (fs::is_symlink(p.parent_path(), ec)) return false;
        }

#ifdef _WIN32
        std::ofstream out(p, std::ios::binary);
#else
        std::ofstream out(path, std::ios::binary);
#endif
        if (!out) return false;

        if (size > 0 && data != nullptr) {
            out.write(data, size);
        }
        out.close();

        if (out.good() && timestamp > 0) {
            auto file_time = unix_to_file_time(timestamp);
            std::error_code ec;
            fs::last_write_time(p, file_time, ec);
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

// ─── 64-BIT SEEK/TELL ───────────────────────────────────────────────────────

bool seek64(FILE* f, int64_t offset, int origin) {
#ifdef _WIN32
    return _fseeki64(f, offset, origin) == 0;
#else
    return fseeko(f, static_cast<off_t>(offset), origin) == 0;
#endif
}

int64_t tell64(FILE* f) {
#ifdef _WIN32
    return _ftelli64(f);
#else
    return static_cast<int64_t>(ftello(f));
#endif
}

// ─── CONVERSIONE TIMESTAMP PORTABILE ─────────────────────────────────────────
// C++17 non garantisce che file_time_type::clock abbia la stessa epoca di
// system_clock. Queste funzioni calcolano l'offset relativo in modo portabile.

uint64_t file_time_to_unix(fs::file_time_type ftime) {
    const auto sys_now  = std::chrono::system_clock::now();
    const auto file_now = fs::file_time_type::clock::now();
    auto offset = ftime - file_now;
    auto sys_time = sys_now + std::chrono::duration_cast<std::chrono::system_clock::duration>(offset);
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(sys_time.time_since_epoch()).count());
}

fs::file_time_type unix_to_file_time(uint64_t timestamp) {
    const auto sys_now  = std::chrono::system_clock::now();
    const auto file_now = fs::file_time_type::clock::now();
    auto sys_time = std::chrono::system_clock::from_time_t(static_cast<time_t>(timestamp));
    auto offset = sys_time - sys_now;
    return file_now + std::chrono::duration_cast<fs::file_time_type::duration>(offset);
}

// ─── DIRECTORY ESEGUIBILE ────────────────────────────────────────────────────

std::string get_exe_directory() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        return fs::path(buf).parent_path().u8string();
    }
    return ".";
#else
    // Prova /proc/self/exe (Linux), poi /proc/curproc/file (FreeBSD), poi fallback
    for (const char* proc_path : {"/proc/self/exe", "/proc/curproc/file"}) {
        std::error_code ec;
        if (fs::exists(proc_path, ec)) {
            auto exe = fs::read_symlink(proc_path, ec);
            if (!ec) return exe.parent_path().string();
        }
    }
    return ".";
#endif
}

} // namespace IO
