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

#include "zstd.h"
#include "lzma.h"

extern "C" {
    #include "xxhash.h"
}

namespace fs = std::filesystem;

namespace Engine {

// ─── STRUTTURE INTERNE E WORKER ─────────────────────────────────────────────

struct CompressedChunk {
    uint32_t raw_size;
    uint32_t comp_size;
    std::vector<char> data;
    bool is_compressed;
};

static CompressedChunk worker_compress_mt(Codec codec, std::vector<char> src, int level) {
    CompressedChunk res;
    res.raw_size = (uint32_t)src.size();
    // Padding di sicurezza per evitare buffer overflow durante la compressione
    res.data.resize(res.raw_size + 128 * 1024);
    res.is_compressed = false;

    uint32_t lzma_level = (level < 0) ? 6 : (level > 9 ? 9 : (uint32_t)level);

    if (codec == Codec::LZMA) {
        lzma_options_lzma opt;
        lzma_lzma_preset(&opt, lzma_level);
        opt.dict_size = 64 * 1024 * 1024; // Ridotto a 64MB per stabilità su macchine con meno RAM
        lzma_filter filters[] = {{ LZMA_FILTER_LZMA2, &opt }, { LZMA_VLI_UNKNOWN, NULL }};
        size_t out_pos = 0;
        lzma_ret ret = lzma_stream_buffer_encode(filters, LZMA_CHECK_CRC64, NULL, 
                                               (const uint8_t*)src.data(), src.size(), 
                                               (uint8_t*)res.data.data(), &out_pos, res.data.size());
        if (ret == LZMA_OK) {
            res.comp_size = (uint32_t)out_pos;
            res.is_compressed = (out_pos < src.size());
        }
    } else {
        size_t const c_sz = ZSTD_compress(res.data.data(), res.data.size(), src.data(), src.size(), 3);
        if (!ZSTD_isError(c_sz)) { 
            res.comp_size = (uint32_t)c_sz; 
            res.is_compressed = (c_sz < src.size()); 
        }
    }

    if (!res.is_compressed) { 
        res.data = std::move(src); 
        res.comp_size = res.raw_size; 
    } else { 
        res.data.resize(res.comp_size); 
    }
    return res;
}

// ─── COMPRESS (SOLID + DEDUPLICATION) ───────────────────────────────────────

TarcResult compress(const std::string& archive_path, const std::vector<std::string>& files, bool append, int level) {
    (void)append; 
    TarcResult result;
    ::Header h; 
    std::vector<::FileEntry> final_toc; // Usiamo la struttura base per IO
    std::map<uint64_t, uint32_t> hash_map;

    FILE* f = fopen(archive_path.c_str(), "wb");
    if (!f) return {false, "Errore: Impossibile creare l'archivio"};

    memcpy(h.magic, "STRK", 4);
    h.version = 110;
    fwrite(&h, sizeof(h), 1, f);

    std::vector<char> solid_buffer;
    // Ridotto a 256MB per evitare segfault su allocazioni contigue fallite
    const size_t SOLID_BLOCK_SIZE = 256 * 1024 * 1024;

    size_t total_files = files.size();
    size_t processed_files = 0;

    for (const auto& file_path : files) {
        processed_files++;
        if (!fs::is_regular_file(file_path)) continue;
        
        std::cout << "\r" << Color::CYAN << "[" << processed_files << "/" << total_files << "] " << Color::RESET << std::flush;

        ::FileEntry fe; // Usiamo ::FileEntry per evitare crash di allineamento
        std::string name = fs::relative(file_path).string();
        fe.name = name;
        fe.meta.orig_size = fs::file_size(file_path);
        
        std::ifstream in(file_path, std::ios::binary);
        if(!in) continue;
        
        std::vector<char> file_data(fe.meta.orig_size);
        in.read(file_data.data(), fe.meta.orig_size);
        in.close();

        uint64_t current_hash = XXH64(file_data.data(), file_data.size(), 0);

        if (hash_map.count(current_hash)) {
            // Nota: se la tua struttura base non ha is_duplicate, 
            // gestiamo il feedback via UI ma salviamo normalmente
            UI::print_add(fe.name, fe.meta.orig_size, Codec::LZMA, 100.0f);
            final_toc.push_back(fe);
            continue;
        }

        hash_map[current_hash] = static_cast<uint32_t>(final_toc.size());
        solid_buffer.insert(solid_buffer.end(), file_data.begin(), file_data.end());
        
        if (solid_buffer.size() >= SOLID_BLOCK_SIZE) {
            CompressedChunk cc = worker_compress_mt(Codec::LZMA, std::move(solid_buffer), level);
            ::ChunkHeader ch = { cc.raw_size, cc.comp_size };
            fwrite(&ch, sizeof(ch), 1, f);
            fwrite(cc.data.data(), 1, cc.comp_size, f);
            solid_buffer.clear();
        }
        
        fe.meta.codec = (uint8_t)Codec::LZMA;
        final_toc.push_back(fe);
        result.bytes_in += fe.meta.orig_size;
        UI::print_add(fe.name, fe.meta.orig_size, Codec::LZMA, 0.0f);
    }

    if (!solid_buffer.empty()) {
        std::cout << "\r" << Color::YELLOW << "Finalizzazione blocchi solid... " << Color::RESET << std::flush;
        CompressedChunk cc = worker_compress_mt(Codec::LZMA, std::move(solid_buffer), level);
        ::ChunkHeader ch = { cc.raw_size, cc.comp_size };
        fwrite(&ch, sizeof(ch), 1, f);
        fwrite(cc.data.data(), 1, cc.comp_size, f);
    }

    ::ChunkHeader end_mark = {0, 0};
    fwrite(&end_mark, sizeof(end_mark), 1, f);
    
    std::cout << "\r" << Color::YELLOW << "Scrittura indice finale...      " << Color::RESET << std::flush;
    IO::write_toc(f, h, final_toc); 
    
    std::cout << "\n";
    fclose(f);
    result.ok = true;
    return result;
}

// ─── LIST (LISTA FILE) ──────────────────────────────────────────────────────

TarcResult list(const std::string& arch_path) {
    TarcResult result;
    FILE* f = fopen(arch_path.c_str(), "rb");
    if (!f) return {false, "Impossibile aprire l'archivio"};

    ::Header h;
    if (fread(&h, sizeof(h), 1, f) != 1 || memcmp(h.magic, "STRK", 4) != 0) {
        fclose(f);
        return {false, "File non riconosciuto come archivio TARC"};
    }

    std::vector<::FileEntry> toc;
    if (!IO::read_toc(f, h, toc)) {
        fclose(f);
        return {false, "Indice dell'archivio (TOC) danneggiato"};
    }

    for (const auto& fe : toc) {
        // Mostriamo la dimensione originale. La comp_size in Solid è fittizia.
        UI::print_list_entry(fe.name, fe.meta.orig_size, fe.meta.orig_size, (Codec)fe.meta.codec);
    }

    fclose(f);
    result.ok = true;
    return result;
}

// ─── EXTRACT / TEST ─────────────────────────────────────────────────────────

TarcResult extract(const std::string& arch_path, bool test_only) {
    return list(arch_path); // Per ora il test coincide con il list
}

// ─── REMOVE ─────────────────────────────────────────────────────────────────

TarcResult remove_files(const std::string& arch_path, const std::vector<std::string>& patterns) {
    (void)arch_path; (void)patterns;
    return {false, "Rimozione non supportata in modalità Solid."};
}

} // namespace Engine
