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
#include <future>
#include <chrono>

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

// Funzione worker per la compressione asincrona
static CompressedChunk worker_compress_async(Codec codec, std::vector<char> src, int level) {
    CompressedChunk res;
    res.raw_size = (uint32_t)src.size();
    if (res.raw_size == 0) return res;

    res.data.resize(res.raw_size + 128 * 1024);
    res.is_compressed = false;

    uint32_t lzma_level = (level < 0) ? 6 : (level > 9 ? 9 : (uint32_t)level);

    if (codec == Codec::LZMA) {
        lzma_options_lzma opt;
        lzma_lzma_preset(&opt, lzma_level);
        opt.dict_size = 64 * 1024 * 1024; 
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

// Supporto Smart Codec: rileva file già compressi
static bool is_already_compressed(const std::string& ext) {
    static const std::vector<std::string> skip = {
        ".pdf", ".zip", ".7z", ".rar", ".gz", ".png", ".jpg", ".jpeg", ".mp4", ".mp3"
    };
    for (const auto& s : skip) {
        if (ext == s) return true;
    }
    return false;
}

// ─── COMPRESS (SOLID + DEDUPLICATION + MULTITHREAD) ─────────────────────────

TarcResult compress(const std::string& archive_path, const std::vector<std::string>& files, bool append, int level) {
    (void)append; 
    TarcResult result;
    ::Header h; 
    std::vector<::FileEntry> final_toc;
    std::map<uint64_t, uint32_t> hash_map;

    // Safe Write: usiamo un file temporaneo
    std::string temp_path = archive_path + ".tmp";
    FILE* f = fopen(temp_path.c_str(), "wb");
    if (!f) return {false, "Errore: Impossibile creare il file temporaneo"};

    memcpy(h.magic, "STRK", 4);
    h.version = 110;
    h.file_count = 0;
    fwrite(&h, sizeof(h), 1, f);

    std::vector<char> solid_buffer;
    const size_t SOLID_BLOCK_SIZE = 256 * 1024 * 1024;
    std::future<CompressedChunk> compress_job;
    bool job_active = false;

    auto finalize_chunk = [&](std::future<CompressedChunk>& job) {
        if (job.valid()) {
            CompressedChunk cc = job.get();
            if (cc.raw_size > 0) {
                ::ChunkHeader ch = { cc.raw_size, cc.comp_size };
                fwrite(&ch, sizeof(ch), 1, f);
                fwrite(cc.data.data(), 1, cc.comp_size, f);
                result.bytes_out += cc.comp_size;
            }
        }
    };

    size_t total_files = files.size();
    auto start_time = std::chrono::steady_clock::now();

    for (size_t i = 0; i < total_files; ++i) {
        const auto& file_path = files[i];
        if (!fs::is_regular_file(file_path)) continue;

        UI::print_progress(i + 1, total_files, fs::path(file_path).filename().string());

        ::FileEntry fe;
        fe.name = fs::relative(file_path).string();
        fe.meta.orig_size = fs::file_size(file_path);
        
        // Metadati: Timestamp
        auto ftime = fs::last_write_time(file_path);
        fe.meta.timestamp = std::chrono::duration_cast<std::chrono::seconds>(ftime.time_since_epoch()).count();

        std::ifstream in(file_path, std::ios::binary);
        if(!in) continue;
        
        std::vector<char> file_data(fe.meta.orig_size);
        in.read(file_data.data(), fe.meta.orig_size);
        in.close();

        // Deduplicazione
        uint64_t current_hash = XXH64(file_data.data(), file_data.size(), 0);
        fe.meta.xxhash = current_hash;

        if (hash_map.count(current_hash)) {
            fe.meta.is_duplicate = 1;
            fe.meta.duplicate_of_idx = hash_map[current_hash];
            fe.meta.comp_size = 0;
            fe.meta.codec = (uint8_t)Codec::NONE;
            final_toc.push_back(fe);
            continue;
        }

        // Nuovo file unico
        hash_map[current_hash] = static_cast<uint32_t>(final_toc.size());
        fe.meta.is_duplicate = 0;
        fe.meta.codec = (uint8_t)Codec::LZMA;
        
        // Smart Codec check (opzionale: potresti forzare NONE se già compresso)
        // if (is_already_compressed(fs::path(file_path).extension().string())) { ... }

        solid_buffer.insert(solid_buffer.end(), file_data.begin(), file_data.end());
        result.bytes_in += fe.meta.orig_size;

        if (solid_buffer.size() >= SOLID_BLOCK_SIZE) {
            if (job_active) finalize_chunk(compress_job);
            compress_job = std::async(std::launch::async, worker_compress_async, Codec::LZMA, std::move(solid_buffer), level);
            solid_buffer.clear();
            job_active = true;
        }
        
        final_toc.push_back(fe);
    }

    // Finalizzazione
    if (job_active) finalize_chunk(compress_job);
    if (!solid_buffer.empty()) {
        CompressedChunk cc = worker_compress_async(Codec::LZMA, std::move(solid_buffer), level);
        ::ChunkHeader ch = { cc.raw_size, cc.comp_size };
        fwrite(&ch, sizeof(ch), 1, f);
        fwrite(cc.data.data(), 1, cc.comp_size, f);
        result.bytes_out += cc.comp_size;
    }

    // Mark end of chunks
    ::ChunkHeader end_mark = {0, 0};
    fwrite(&end_mark, sizeof(end_mark), 1, f);
    
    // Scrittura Indice
    h.file_count = static_cast<uint32_t>(final_toc.size());
    IO::write_toc(f, h, final_toc); 
    
    fclose(f);

    // Swap file temporaneo
    fs::rename(temp_path, archive_path);
    
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

    std::cout << "\n" << Color::BOLD << Color::WHITE << " Contenuto di: " << arch_path << Color::RESET << "\n";
    std::cout << " --------------------------------------------------------------------------\n";

    for (const auto& fe : toc) {
        uint64_t csize = fe.meta.is_duplicate ? 0 : fe.meta.orig_size; 
        UI::print_list_entry(fe.name, fe.meta.orig_size, csize, (Codec)fe.meta.codec);
    }

    fclose(f);
    result.ok = true;
    return result;
}

// ─── EXTRACT / TEST ─────────────────────────────────────────────────────────

TarcResult extract(const std::string& arch_path, bool test_only) {
    (void)test_only;
    // L'estrazione Solid richiede la lettura sequenziale dei blocchi e il dispatch dei file
    // Implementazione prevista per la Fase 3
    return list(arch_path); 
}

// ─── REMOVE ─────────────────────────────────────────────────────────────────

TarcResult remove_files(const std::string& arch_path, const std::vector<std::string>& patterns) {
    (void)arch_path; (void)patterns;
    return {false, "La rimozione in modalità Solid richiede la ricostruzione dell'archivio."};
}

} // namespace Engine
