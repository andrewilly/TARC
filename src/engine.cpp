#include "engine.h"
#include "io.h"
#include "ui.h"
#include "types.h"
#include <cstring>
#include <map>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <vector>
#include <iostream>
#include <chrono>

#include "zstd.h"
#include "lzma.h"

extern "C" {
    #include "xxhash.h"
}

namespace fs = std::filesystem;

namespace Engine {

TarcResult compress(const std::string& archive_path, const std::vector<std::string>& files, bool append, int level) {
    (void)append;
    TarcResult result;
    ::Header h;
    std::vector<::FileEntry> final_toc;
    std::map<uint64_t, uint32_t> hash_map;

    FILE* f = fopen((archive_path + ".tmp").c_str(), "wb");
    if (!f) return {false, "Errore apertura file"};

    memcpy(h.magic, "STRK", 4);
    h.version = 110;
    fwrite(&h, sizeof(h), 1, f);

    std::vector<char> solid_buf;
    const size_t MAX_SOLID = 64 * 1024 * 1024; // 64MB per chunk

    auto flush_solid = [&]() {
        if (solid_buf.empty()) return;
        
        std::vector<char> comp_buf(solid_buf.size() + 64 * 1024);
        lzma_options_lzma opt;
        lzma_lzma_preset(&opt, (uint32_t)((level < 0) ? 6 : level));
        lzma_filter filters[] = {{ LZMA_FILTER_LZMA2, &opt }, { LZMA_VLI_UNKNOWN, NULL }};
        size_t out_pos = 0;
        
        lzma_ret ret = lzma_stream_buffer_encode(filters, LZMA_CHECK_CRC64, NULL, 
                                               (uint8_t*)solid_buf.data(), solid_buf.size(), 
                                               (uint8_t*)comp_buf.data(), &out_pos, comp_buf.size());

        if (ret == LZMA_OK) {
            ::ChunkHeader ch = { (uint32_t)solid_buf.size(), (uint32_t)out_pos };
            fwrite(&ch, sizeof(ch), 1, f);
            fwrite(comp_buf.data(), 1, out_pos, f);
            result.bytes_out += out_pos;
        }
        solid_buf.clear();
    };

    for (size_t i = 0; i < files.size(); ++i) {
        if (!fs::is_regular_file(files[i])) continue;
        UI::print_progress(i + 1, files.size(), fs::path(files[i]).filename().string());

        ::FileEntry fe;
        fe.name = fs::relative(files[i]).string();
        fe.meta.orig_size = fs::file_size(files[i]);
        auto ftime = fs::last_write_time(files[i]);
        fe.meta.timestamp = std::chrono::duration_cast<std::chrono::seconds>(ftime.time_since_epoch()).count();

        std::ifstream in(files[i], std::ios::binary);
        std::vector<char> data(fe.meta.orig_size);
        in.read(data.data(), fe.meta.orig_size);
        in.close();

        uint64_t h64 = XXH64(data.data(), data.size(), 0);
        fe.meta.xxhash = h64;

        if (hash_map.count(h64)) {
            fe.meta.is_duplicate = 1;
            fe.meta.duplicate_of_idx = hash_map[h64];
        } else {
            hash_map[h64] = (uint32_t)final_toc.size();
            fe.meta.is_duplicate = 0;
            solid_buf.insert(solid_buf.end(), data.begin(), data.end());
            result.bytes_in += fe.meta.orig_size;
            if (solid_buf.size() >= MAX_SOLID) flush_solid();
        }
        fe.meta.codec = (uint8_t)Codec::LZMA;
        final_toc.push_back(fe);
    }

    flush_solid();
    ::ChunkHeader end = {0, 0};
    fwrite(&end, sizeof(end), 1, f);
    
    IO::write_toc(f, h, final_toc);
    fclose(f);
    fs::rename(archive_path + ".tmp", archive_path);
    
    result.ok = true;
    return result;
}

TarcResult extract(const std::string& arch_path, bool test_only) {
    TarcResult result;
    FILE* f = fopen(arch_path.c_str(), "rb");
    if (!f) return {false, "Impossibile aprire file"};

    ::Header h;
    if (fread(&h, sizeof(h), 1, f) != 1) { fclose(f); return {false, "Header invalido"}; }

    std::vector<::FileEntry> toc;
    if (!IO::read_toc(f, h, toc)) { fclose(f); return {false, "TOC invalida"}; }

    fseek(f, sizeof(::Header), SEEK_SET);
    std::vector<char> current_block;
    size_t cursor = 0;

    for (size_t i = 0; i < toc.size(); ++i) {
        auto& fe = toc[i];
        UI::print_progress(i + 1, toc.size(), fe.name);

        if (fe.meta.is_duplicate) {
            if (!test_only) {
                try { fs::copy(toc[fe.meta.duplicate_of_idx].name, fe.name, fs::copy_options::overwrite_existing); } catch(...) {}
            }
            continue;
        }

        // Se il cursore ha superato il blocco attuale, carichiamo il prossimo chunk dal file
        while (cursor + fe.meta.orig_size > current_block.size()) {
            // Se avevamo dati residui piccoli, li scartiamo (i file non sono spezzati tra chunk in questa versione)
            if (cursor < current_block.size() && cursor > 0) {
                 current_block.erase(current_block.begin(), current_block.begin() + cursor);
                 cursor = 0;
            }

            ::ChunkHeader ch;
            if (fread(&ch, sizeof(ch), 1, f) != 1 || ch.raw_size == 0) break;

            std::vector<char> comp(ch.comp_size);
            fread(comp.data(), 1, ch.comp_size, f);

            std::vector<char> decomp(ch.raw_size);
            size_t sp = 0, dp = 0;
            lzma_stream_buffer_decode(NULL, UINT64_MAX, NULL, (uint8_t*)comp.data(), &sp, ch.comp_size, (uint8_t*)decomp.data(), &dp, ch.raw_size);
            
            // Aggiungiamo il nuovo blocco a quello esistente
            current_block.insert(current_block.end(), decomp.begin(), decomp.end());
        }

        if (cursor + fe.meta.orig_size > current_block.size()) {
            fclose(f); return {false, "Errore: Dati mancanti per " + fe.name};
        }

        const char* ptr = current_block.data() + cursor;
        if (XXH64(ptr, fe.meta.orig_size, 0) != fe.meta.xxhash) {
            fclose(f); return {false, "FALLITO TEST INTEGRITÀ: " + fe.name};
        }

        if (!test_only) IO::write_file_to_disk(fe.name, ptr, fe.meta.orig_size, fe.meta.timestamp);
        
        cursor += fe.meta.orig_size;
        result.bytes_out += fe.meta.orig_size;
    }

    fclose(f);
    result.ok = true;
    return result;
}

TarcResult list(const std::string& arch_path) {
    TarcResult res;
    FILE* f = fopen(arch_path.c_str(), "rb");
    if (!f) return {false, "Errore"};
    ::Header h; fread(&h, sizeof(h), 1, f);
    std::vector<::FileEntry> toc;
    IO::read_toc(f, h, toc);
    std::cout << "\n--- ARCHIVIO: " << arch_path << " ---\n";
    for (const auto& fe : toc) UI::print_list_entry(fe.name, fe.meta.orig_size, 0, (Codec)fe.meta.codec);
    fclose(f);
    res.ok = true;
    return res;
}

TarcResult remove_files(const std::string& a, const std::vector<std::string>& p) { return {false, "Non supportato"}; }

} // namespace Engine
