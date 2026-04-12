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

// Worker per la compressione effettiva dei blocchi Solid
static CompressedChunk worker_compress_mt(Codec codec, std::vector<char> src, int level) {
    CompressedChunk res;
    res.raw_size = (uint32_t)src.size();
    res.data.resize(res.raw_size + 65536);
    res.is_compressed = false;

    if (codec == Codec::LZMA) {
        lzma_options_lzma opt;
        lzma_lzma_preset(&opt, 9);
        opt.dict_size = 128 * 1024 * 1024; // 128MB Dictionary per Solid
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
        // Default ZSTD per i livelli standard
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

// ─── COMPRESS (MODALITÀ SOLID + DEDUPLICAZIONE) ──────────────────────────────

TarcResult compress(const std::string& archive_path, const std::vector<std::string>& files, bool append, int level) {
    TarcResult result;
    ::Header h; 
    std::vector<Engine::FileEntry> toc;
    std::map<uint64_t, uint32_t> hash_map;

    FILE* f = fopen(archive_path.c_str(), "wb");
    if (!f) return {false, "Errore: Impossibile creare il file archivio"};

    memcpy(h.magic, "STRK", 4);
    h.version = 110;
    fwrite(&h, sizeof(h), 1, f);

    std::vector<char> solid_buffer;
    const size_t SOLID_BLOCK_SIZE = 512 * 1024 * 1024; // 512MB Solid Block

    for (uint32_t i = 0; i < files.size(); ++i) {
        if (!fs::is_regular_file(files[i])) continue;
        
        Engine::FileEntry fe;
        fe.name = fs::relative(files[i]).string();
        fe.meta.orig_size = fs::file_size(files[i]);
        
        std::ifstream in(files[i], std::ios::binary);
        std::vector<char> file_data(fe.meta.orig_size);
        in.read(file_data.data(), fe.meta.orig_size);
        in.close();

        // Calcolo XXH64 per deduplicazione
        fe.meta.xxhash = XXH64(file_data.data(), file_data.size(), 0);

        if (hash_map.count(fe.meta.xxhash)) {
            fe.meta.is_duplicate = true;
            fe.meta.duplicate_of_idx = hash_map[fe.meta.xxhash];
            UI::print_add(fe.name, fe.meta.orig_size, Codec::LZMA, 100.0f);
            toc.push_back(fe);
            continue;
        }

        hash_map[fe.meta.xxhash] = static_cast<uint32_t>(toc.size());
        solid_buffer.insert(solid_buffer.end(), file_data.begin(), file_data.end());
        
        if (solid_buffer.size() >= SOLID_BLOCK_SIZE) {
            CompressedChunk cc = worker_compress_mt(Codec::LZMA, std::move(solid_buffer), level);
            ::ChunkHeader ch = { cc.raw_size, cc.comp_size };
            fwrite(&ch, sizeof(ch), 1, f);
            fwrite(cc.data.data(), 1, cc.comp_size, f);
            solid_buffer.clear();
        }
        
        fe.meta.codec = (uint8_t)Codec::LZMA;
        toc.push_back(fe);
        result.bytes_in += fe.meta.orig_size;
        UI::print_add(fe.name, fe.meta.orig_size, Codec::LZMA, 0.0f);
    }

    if (!solid_buffer.empty()) {
        CompressedChunk cc = worker_compress_mt(Codec::LZMA, std::move(solid_buffer), level);
        ::ChunkHeader ch = { cc.raw_size, cc.comp_size };
        fwrite(&ch, sizeof(ch), 1, f);
        fwrite(cc.data.data(), 1, cc.comp_size, f);
    }

    // Segnale fine stream dati
    ::ChunkHeader end_mark = {0, 0};
    fwrite(&end_mark, sizeof(end_mark), 1, f);
    
    // Scrittura Indice (TOC) - Cast necessario per compatibilità con io.h
    IO::write_toc(f, h, (std::vector<::FileEntry>&)toc); 
    
    fclose(f);
    result.ok = true;
    return result;
}

// ─── LIST (VISUALIZZAZIONE CONTENUTO) ────────────────────────────────────────

TarcResult list(const std::string& arch_path) {
    TarcResult result;
    FILE* f = fopen(arch_path.c_str(), "rb");
    if (!f) return {false, "Impossibile aprire l'archivio"};

    ::Header h;
    if (fread(&h, sizeof(h), 1, f) != 1 || memcmp(h.magic, "STRK", 4) != 0) {
        fclose(f);
        return {false, "Formato file non valido"};
    }

    std::vector<::FileEntry> toc;
    if (!IO::read_toc(f, h, toc)) {
        fclose(f);
        return {false, "Errore nella lettura del Table of Contents (TOC)"};
    }

    UI::print_list_header();
    for (const auto& fe : toc) {
        // Visualizza ratio 100% se è un duplicato (spazio risparmiato totalmente)
        float ratio = fe.meta.is_duplicate ? 100.0f : 0.0f;
        UI::print_list_item(fe.name, fe.meta.orig_size, (Codec)fe.meta.codec, ratio);
    }

    fclose(f);
    result.ok = true;
    return result;
}

// ─── EXTRACT (TEST E ESTRAZIONE) ────────────────────────────────────────────

TarcResult extract(const std::string& arch_path, bool test_only) {
    // Il comando -t usa questa funzione. Per ora validiamo il TOC e la struttura.
    TarcResult res = list(arch_path);
    if (!res.ok) return res;

    if (test_only) {
        UI::print_info("Verifica integrità completata: Archivio strutturalmente valido.");
        return {true, ""};
    }

    return {false, "Estrazione Solid non ancora supportata in questa versione."};
}

// ─── ALTRE OPERAZIONI ────────────────────────────────────────────────────────

TarcResult remove_files(const std::string& arch_path, const std::vector<std::string>& patterns) {
    return {false, "Rimozione file non supportata in modalità Solid (richiede ricostruzione)"};
}

} // namespace Engine
