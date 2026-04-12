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
#include <thread>

#include "zstd.h"
#include "lz4.h"
#include "lzma.h"

extern "C" {
    #include "xxhash.h"
}

namespace fs = std::filesystem;

namespace Engine {

// Struttura interna per gestire i blocchi compressi in memoria
struct CompressedChunk {
    uint32_t raw_size;
    uint32_t comp_size;
    std::vector<char> data;
    bool is_compressed;
};

// Genera dati di parità semplici (XOR) per il recupero errori (5% default)
std::vector<char> generate_parity_data(const std::vector<char>& data, float ratio = 0.05f) {
    size_t parity_size = static_cast<size_t>(data.size() * ratio);
    if (parity_size == 0) parity_size = 1;
    std::vector<char> parity(parity_size, 0);
    for (size_t i = 0; i < data.size(); ++i) {
        parity[i % parity_size] ^= data[i];
    }
    return parity;
}

// Funzione di compressione effettiva (Worker)
static CompressedChunk worker_compress_mt(Codec codec, std::vector<char> src, int level) {
    CompressedChunk res;
    res.raw_size = (uint32_t)src.size();
    // Buffer di sicurezza: dimensione originale + 64KB
    res.data.resize(res.raw_size + 65536);
    res.is_compressed = false;

    if (codec == Codec::LZMA) {
        lzma_options_lzma opt;
        lzma_lzma_preset(&opt, 9);
        opt.dict_size = 128 * 1024 * 1024; // 128MB Dictionary per Solid Mode
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
        // Default a ZSTD per velocità se non Ultra
        size_t const c_sz = ZSTD_compress(res.data.data(), res.data.size(), src.data(), src.size(), level >= 9 ? 19 : 3);
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

// --- FUNZIONE COMPRESS PRINCIPALE ---
TarcResult compress(const std::string& archive_path, const std::vector<std::string>& files, bool append, int level) {
    TarcResult result;
    ::Header h; // Usa Header dal namespace globale (types.h)
    std::vector<FileEntry> toc;
    std::map<uint64_t, uint32_t> hash_map;

    FILE* f = fopen(archive_path.c_str(), "wb");
    if (!f) return {false, "Impossibile creare l'archivio"};

    memcpy(h.magic, "STRK", 4);
    h.version = 110;
    fwrite(&h, sizeof(h), 1, f);

    std::vector<char> solid_buffer;
    const size_t SOLID_BLOCK_SIZE = 512 * 1024 * 1024; // 512MB per blocco Solid

    for (uint32_t i = 0; i < files.size(); ++i) {
        if (!fs::is_regular_file(files[i])) continue;
        
        FileEntry fe;
        fe.name = fs::relative(files[i]).string();
        fe.meta.orig_size = fs::file_size(files[i]);
        
        std::ifstream in(files[i], std::ios::binary);
        std::vector<char> file_data(fe.meta.orig_size);
        in.read(file_data.data(), fe.meta.orig_size);
        in.close();

        // Calcolo Hash per deduplicazione
        fe.meta.xxhash = XXH64(file_data.data(), file_data.size(), 0);

        // Controllo Deduplicazione
        if (hash_map.count(fe.meta.xxhash)) {
            fe.meta.is_duplicate = true;
            fe.meta.duplicate_of_idx = hash_map[fe.meta.xxhash];
            UI::print_add(fe.name, fe.meta.orig_size, Codec::LZMA, 100.0f); // Ratio 100% (risparmiato tutto)
            toc.push_back(fe);
            continue;
        }

        // Se non è duplicato, aggiungi al buffer solid e mappa l'hash
        hash_map[fe.meta.xxhash] = static_cast<uint32_t>(toc.size());
        solid_buffer.insert(solid_buffer.end(), file_data.begin(), file_data.end());
        
        // Se raggiungiamo la dimensione del blocco, comprimiamo
        if (solid_buffer.size() >= SOLID_BLOCK_SIZE) {
            CompressedChunk cc = worker_compress_mt(Codec::LZMA, std::move(solid_buffer), level);
            ::ChunkHeader ch = { cc.raw_size, cc.comp_size };
            fwrite(&ch, sizeof(ch), 1, f);
            fwrite(cc.data.data(), 1, cc.comp_size, f);
            
            // Scrittura parità
            auto parity = generate_parity_data(cc.data, 0.05f);
            uint32_t p_size = (uint32_t)parity.size();
            fwrite(&p_size, sizeof(p_size), 1, f);
            fwrite(parity.data(), 1, p_size, f);
            
            solid_buffer.clear();
        }
        
        toc.push_back(fe);
        result.bytes_in += fe.meta.orig_size;
        UI::print_add(fe.name, fe.meta.orig_size, Codec::LZMA, 0.0f);
    }

    // Comprimi l'ultimo residuo del buffer solid
    if (!solid_buffer.empty()) {
        CompressedChunk cc = worker_compress_mt(Codec::LZMA, std::move(solid_buffer), level);
        ::ChunkHeader ch = { cc.raw_size, cc.comp_size };
        fwrite(&ch, sizeof(ch), 1, f);
        fwrite(cc.data.data(), 1, cc.comp_size, f);
        
        auto parity = generate_parity_data(cc.data, 0.05f);
        uint32_t p_size = (uint32_t)parity.size();
        fwrite(&p_size, sizeof(p_size), 1, f);
        fwrite(parity.data(), 1, p_size, f);
    }

    // Marker fine blocchi (raw_size = 0)
    ::ChunkHeader end_mark = {0, 0};
    fwrite(&end_mark, sizeof(end_mark), 1, f);

    // Scrittura Table of Contents finale
    IO::write_toc(f, h, toc);
    
    fclose(f);
    result.ok = true;
    return result;
}

// --- FUNZIONI DI SERVIZIO (STUB) ---

TarcResult extract(const std::string& arch_path, bool test_only) {
    // Implementazione futura: lettura sequenziale blocchi solid e ripristino file
    return {true, "Funzione estrazione in fase di migrazione Solid"};
}

TarcResult list(const std::string& arch_path) {
    // Implementazione futura: lettura TOC finale
    return {true, "Funzione lista in fase di migrazione Solid"};
}

TarcResult remove_files(const std::string& arch_path, const std::vector<std::string>& patterns) {
    return {false, "Rimozione non supportata in modalità Solid senza ricostruzione"};
}

} // namespace Engine
