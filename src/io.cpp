#include "io.h"
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

namespace IO {
    bool read_bytes(FILE* f, void* buf, size_t size) { return fread(buf, 1, size, f) == size; }
    bool write_bytes(FILE* f, const void* buf, size_t size) { return fwrite(buf, 1, size, f) == size; }

    bool read_toc(FILE* f, Header& h, std::vector<FileEntry>& toc) {
        if (!f) return false;
        rewind(f);
        if (!read_bytes(f, &h, sizeof(Header))) return false;
        if (memcmp(h.magic, TARC_MAGIC, 4) != 0) return false;
        fseek(f, (long)h.toc_offset, SEEK_SET);
        uint32_t count = 0;
        if (!read_bytes(f, &count, sizeof(count))) return false;
        toc.clear();
        for (uint32_t i = 0; i < count; ++i) {
            FileEntry fe;
            if (h.version <= 103) {
                struct LegacyEntry { uint64_t o, os, cs, h; uint16_t nl; uint8_t c, p; } l;
                read_bytes(f, &l, sizeof(l));
                fe.meta = {l.o, l.os, l.cs, l.h, 0, l.nl, l.c, 0};
            } else {
                read_bytes(f, &fe.meta, sizeof(Entry));
            }
            fe.name.resize(fe.meta.name_len);
            read_bytes(f, fe.name.data(), fe.meta.name_len);
            toc.push_back(std::move(fe));
        }
        return true;
    }

    void write_toc(FILE* f, Header& h, std::vector<FileEntry>& toc) {
        h.toc_offset = ftell(f);
        uint32_t count = (uint32_t)toc.size();
        write_bytes(f, &count, sizeof(count));
        for (auto& fe : toc) {
            write_bytes(f, &fe.meta, sizeof(Entry));
            write_bytes(f, fe.name.data(), fe.name.size());
        }
        rewind(f);
        write_bytes(f, &h, sizeof(h));
    }
}
