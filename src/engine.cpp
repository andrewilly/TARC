#include "engine.h"
#include "io.h"
#include "ui.h"
#include <zstd.h>
#include <lz4.h>
#include <xxhash.h>
#include <filesystem>
#include <algorithm>

namespace fs = std::filesystem;

namespace CodecSelector {
    double get_entropy(const char* data, size_t sz) {
        if (sz == 0) return 0;
        size_t freq[256] = {0};
        for (size_t i = 0; i < sz; ++i) freq[(uint8_t)data[i]]++;
        double ent = 0;
        for (int i = 0; i < 256; ++i) {
            if (freq[i] > 0) {
                double p = (double)freq[i] / sz;
                ent -= p * (log(p) / log(2));
            }
        }
        return ent;
    }
}

namespace Engine {
    TarcResult compress(const std::string& arch_path, const std::vector<std::string>& files, bool append, int level) {
        TarcResult result;
        std::vector<FileEntry> toc;
        Header h{{'T','A','R','C'}, TARC_VERSION, 0};
        
        FILE* out = fopen(arch_path.c_str(), append ? "rb+" : "wb");
        if (append) {
            Header old_h;
            IO::read_toc(out, old_h, toc);
            fseek(out, 0, SEEK_END);
        } else {
            IO::write_bytes(out, &h, sizeof(h));
        }

        ZSTD_CCtx* cctx = ZSTD_createCCtx();
        for (const auto& f : files) {
            if (!fs::exists(f) || fs::is_directory(f)) continue;
            
            uint64_t ts = fs::last_write_time(f).time_since_epoch().count();
            uint64_t sz = fs::file_size(f);

            // SMART UPDATE CHECK
            bool skip = false;
            for(auto& e : toc) {
                if(e.name == f && e.meta.timestamp == ts && e.meta.orig_size == sz) {
                    skip = true; break;
                }
            }
            if(skip) { UI::print_warning("Skipped (Up-to-date): " + f); continue; }

            // Logica di compressione... (omessa per brevità ma inclusa nel concetto di chunking v1.03)
            // [Qui inseriresti il loop di lettura file e ZSTD_compress come nel tuo originale]
            // Al termine, aggiungi alla TOC
        }
        IO::write_toc(out, h, toc);
        fclose(out);
        ZSTD_freeCCtx(cctx);
        return result;
    }
}
