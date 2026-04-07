#include "io.h"
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

namespace IO {

// ─── HELPERS ─────────────────────────────────────────────────────────────────
bool read_bytes(FILE* f, void* buf, size_t size) {
    return fread(buf, 1, size, f) == size;
}

bool write_bytes(FILE* f, const void* buf, size_t size) {
    return fwrite(buf, 1, size, f) == size;
}

// ─── ESTENSIONE ──────────────────────────────────────────────────────────────
std::string ensure_ext(const std::string& path) {
    const std::string ext = TARC_EXT;
    if (path.size() < ext.size() ||
        path.compare(path.size() - ext.size(), ext.size(), ext) != 0)
        return path + ext;
    return path;
}

// ─── EXPAND PATH (con wildcard) ──────────────────────────────────────────────
void expand_path(const std::string& pattern, std::vector<std::string>& out) {
    fs::path p(pattern);
    fs::path dir    = p.has_parent_path() ? p.parent_path() : fs::path(".");
    std::string fn  = p.filename().string();

    if (fn.find('*') != std::string::npos) {
        // Estrai il suffisso dal wildcard (es. "*.cpp" → ".cpp")
        std::string suffix;
        if (fn.size() > 1 && fn[0] == '*') suffix = fn.substr(1);
        else if (fn == "*")                suffix = "";
        else                               suffix = fn;

        if (fs::exists(dir) && fs::is_directory(dir)) {
            for (const auto& entry : fs::directory_iterator(dir)) {
                if (entry.is_directory()) continue;
                std::string fname = entry.path().filename().string();
                if (suffix.empty() ||
                    (fname.size() >= suffix.size() &&
                     fname.compare(fname.size() - suffix.size(),
                                   suffix.size(), suffix) == 0))
                    out.push_back(entry.path().string());
            }
        }
    } else if (fs::exists(p)) {
        if (fs::is_directory(p)) {
            for (auto& item : fs::recursive_directory_iterator(p))
                if (!item.is_directory())
                    out.push_back(item.path().string());
        } else {
            out.push_back(p.string());
        }
    }
}

// ─── TOC READ ────────────────────────────────────────────────────────────────
bool read_toc(FILE* f, Header& h, std::vector<FileEntry>& toc) {
    if (!f) return false;

    rewind(f);
    if (!read_bytes(f, &h, sizeof(Header))) return false;
    if (memcmp(h.magic, TARC_MAGIC, 4) != 0) return false;

    if (fseek(f, static_cast<long>(h.toc_offset), SEEK_SET) != 0) return false;

    uint32_t count = 0;
    if (!read_bytes(f, &count, sizeof(count))) return false;

    toc.clear();
    toc.reserve(count);

    for (uint32_t i = 0; i < count; ++i) {
        FileEntry fe;
        if (!read_bytes(f, &fe.meta, sizeof(Entry))) break;
        fe.name.resize(fe.meta.name_len);
        if (!read_bytes(f, fe.name.data(), fe.meta.name_len)) break;
        toc.push_back(std::move(fe));
    }
    return true;
}

// ─── TOC WRITE ───────────────────────────────────────────────────────────────
void write_toc(FILE* f, Header& h, std::vector<FileEntry>& toc) {
    h.toc_offset = static_cast<uint64_t>(ftell(f));
    uint32_t count = static_cast<uint32_t>(toc.size());
    write_bytes(f, &count, sizeof(count));

    for (auto& fe : toc) {
        write_bytes(f, &fe.meta, sizeof(Entry));
        write_bytes(f, fe.name.data(), fe.meta.name_len);
    }

    // Aggiorna header all'inizio del file
    rewind(f);
    write_bytes(f, &h, sizeof(Header));
}

} // namespace IO
