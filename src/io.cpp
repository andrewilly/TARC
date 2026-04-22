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

#ifdef _WIN32
    #include <windows.h>
#endif

namespace fs = std::filesystem;

namespace IO {

// ═══════════════════════════════════════════════════════════════════════════════
// SICUREZZA PERCORSI — Intervento #11
// Previene Path Traversal: un archivio malevolo potrebbe contenere percorsi
// come "../../etc/passwd" o percorsi assoluti che scrivono fuori dalla
// directory di estrazione. Questa funzione:
// 1. Rifiuta percorsi assoluti (C:\, /, \)
// 2. Rimuove componenti ".." (parent directory traversal)
// 3. Normalizza i separatori a /
// 4. Rifiuta percorsi che tentano di uscire dalla radice relativa
// ═══════════════════════════════════════════════════════════════════════════════

std::string sanitize_path(const std::string& path) {
    if (path.empty()) return "";

    // Rifiuta percorsi assoluti
#ifdef _WIN32
    // C:\, \\, X:\, ecc.
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
            // Se non possiamo tornare indietro (nessun componente), rifiuta
            if (parts.empty()) return "";
            parts.pop_back();
        } else {
            // Rifiuta componenti con caratteri pericolosi
            // (caratteri non stampabili, null byte, ecc.)
            for (char c : part) {
                if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) return "";
            }
            parts.push_back(part);
        }
    }

    if (parts.empty()) return "";

    // Ricostruisci il percorso sicuro
    std::string safe_path;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) safe_path += '/';
        safe_path += parts[i];
    }
    return safe_path;
}

// ═══════════════════════════════════════════════════════════════════════════════
// VALIDAZIONE HEADER — Intervento #13
// Verifica che un file sia un archivio TARC valido prima di eseguire append.
// Controlla il magic number e la versione per compatibilita'.
// ═══════════════════════════════════════════════════════════════════════════════

bool validate_header(const Header& h) {
    // Verifica magic number
    if (memcmp(h.magic, TARC_MAGIC, 4) != 0) return false;

    // Verifica versione: deve essere tra 200 e TARC_VERSION (corrente)
    // Questo permette append da archivi v200 in poi
    if (h.version < 200 || h.version > TARC_VERSION) return false;

    // Verifica sanity check: toc_offset deve essere almeno sizeof(Header)
    if (h.toc_offset > 0 && h.toc_offset < sizeof(Header)) return false;

    return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// SCRITTURE ATOMICHE — Intervento #12
// Genera un nome file temporaneo nella stessa directory del target,
// cosi' la rename() finale e' atomica (stesso filesystem).
// ═══════════════════════════════════════════════════════════════════════════════

std::string make_temp_path(const std::string& target_path) {
    // Genera 8 caratteri hex randomici per unicita'
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);
    uint32_t r = dist(gen);

    char hex[9];
    snprintf(hex, sizeof(hex), "%08x", r);

    return target_path + ".tmp" + hex;
}

bool atomic_rename(const std::string& from, const std::string& to) {
#ifdef _WIN32
    // MoveFileExA con MOVEFILE_REPLACE_EXISTING e' atomica su NTFS
    return MoveFileExA(from.c_str(), to.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    // rename() e' atomica su POSIX (stesso filesystem)
    return rename(from.c_str(), to.c_str()) == 0;
#endif
}

bool safe_remove(const std::string& path) {
    std::error_code ec;
    fs::remove(path, ec);
    return !ec;
}

// ═══════════════════════════════════════════════════════════════════════════════
// FUNZIONI I/O ESISTENTI
// ═══════════════════════════════════════════════════════════════════════════════

// Garantisce che l'estensione sia sempre .strk
std::string ensure_ext(const std::string& path) {
    if (path.length() < 5 || path.substr(path.length() - 5) != ".strk") {
        return path + ".strk";
    }
    return path;
}

// Espansione percorsi con supporto directory ricorsivo
void expand_path(const std::string& pattern, std::vector<std::string>& out) {
    if (fs::exists(pattern)) {
        if (fs::is_directory(pattern)) {
            for (auto& p : fs::recursive_directory_iterator(pattern))
                if (p.is_regular_file()) out.push_back(p.path().string());
        } else {
            out.push_back(pattern);
        }
    }
}

// Legge il catalogo (TOC) dall'archivio
bool read_toc(FILE* f, Header& h, std::vector<FileEntry>& toc) {
    if (h.toc_offset == 0) return false;

    if (!seek64(f, static_cast<int64_t>(h.toc_offset), SEEK_SET)) return false;

    toc.clear();
    toc.reserve(h.file_count);

    for (uint32_t i = 0; i < h.file_count; ++i) {
        FileEntry fe;
        if (fread(&fe.meta, sizeof(Entry), 1, f) != 1) return false;

        // Protezione contro name_len corrotto (usa costante nominata)
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
            std::min(static_cast<size_t>(fe.name.length()), static_cast<size_t>(MAX_NAME_LEN)));
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

// --- 64-BIT SEEK/TELL ---

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

} // namespace IO
