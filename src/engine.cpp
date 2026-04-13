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

struct CompressedChunk {
    uint32_t raw_size;
    uint32_t comp_size;
    std::vector<char> data;
    bool is_compressed;
};

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

TarcResult compress(const std::string& archive_path, const std::vector<std::string>& files, bool append, int level) {
    (void)append; 
    TarcResult result;
    ::Header h; 
    std::vector<::FileEntry> final_toc;
    std::map<uint64_t, uint32_t> hash_map;

    std::string temp_path = archive_path + ".tmp";
    FILE* f = fopen(temp_path.c_str(), "wb");
    if (!f) return {false, "Errore: Impossibile creare il file temporaneo"};

    memcpy(h.magic, "STRK", 4);
    h.version = 110;
    fwrite(&h, sizeof(h), 1, f);

    std::vector<char> solid_buffer;
    const size_t SOLID_BLOCK_SIZE = 128 * 1024 * 1024; // Ridotto per stabilità

    auto write_chunk_to_file = [&](CompressedChunk& cc) {
        if (cc.raw_size > 0) {
            ::ChunkHeader ch = { cc.raw_size, cc.comp_size };
            fwrite(&ch, sizeof(ch), 1, f);
            fwrite(cc.data.data(), 1, cc.comp_size, f);
            result.bytes_out += cc.comp_size;
        }
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
        std::vector<char> file_data(fe.meta.orig_size);
        in.read(file_data.data(), fe.meta.orig_size);
        in.close();

        uint64_t current_hash = XXH64(file_data.data(), file_data.size(), 0);
        fe.meta.xxhash = current_hash;

        if (hash_map.count(current_hash)) {
            fe.meta.is_duplicate = 1;
            fe.meta.duplicate_of_idx = hash_map[current_hash];
            final_toc.push_back(fe);
            continue;
        }

        hash_map[current_hash] = static_cast<uint32_t>(final_toc.size());
        fe.meta.is_duplicate = 0;
        fe.meta.codec = (uint8_t)Codec::LZMA;

        solid_buffer.insert(solid_buffer.end(), file_data.begin(), file_data.end());
        result.bytes_in += fe.meta.orig_size;

        if (solid_buffer.size() >= SOLID_BLOCK_SIZE) {
            CompressedChunk cc = worker_compress_async(Codec::LZMA, std::move(solid_buffer), level);
            write_chunk_to_file(cc);
            solid_buffer.clear();
        }
        final_toc.push_back(fe);
    }

    if (!solid_buffer.empty()) {
        CompressedChunk cc = worker_compress_async(Codec::LZMA, std::move(solid_buffer), level);
        write_chunk_to_file(cc);
    }

    ::ChunkHeader end_mark = {0, 0};
    fwrite(&end_mark, sizeof(end_mark), 1, f);
    
    IO::write_toc(f, h, final_toc); 
    fclose(f);
    fs::rename(temp_path, archive_path);
    
    result.ok = true;
    return result;
}

TarcResult extract(const std::string& arch_path, bool test_only) {
    TarcResult result;
    FILE* f = fopen(arch_path.c_str(), "rb");
    if (!f) return {false, "Impossibile aprire l'archivio"};

    ::Header h;
    if (fread(&h, sizeof(h), 1, f) != 1) { fclose(f); return {false, "Header corrotto"}; }

    std::vector<::FileEntry> toc;
    if (!IO::read_toc(f, h, toc)) { fclose(f); return {false, "Indice corrotto"}; }

    fseek(f, sizeof(::Header), SEEK_SET);

    std::vector<char> block;
    size_t block_cursor = 0;

    for (size_t i = 0; i < toc.size(); ++i) {
        auto& fe = toc[i];
        UI::print_progress(i + 1, toc.size(), fe.name);

        if (fe.meta.is_duplicate) {
            if (!test_only) {
                try { fs::copy(toc[fe.meta.duplicate_of_idx].name, fe.name, fs::copy_options::overwrite_existing); } catch(...) {}
            }
            continue;
        }

        // Caricamento blocco se necessario
        while (block_cursor + fe.meta.orig_size > block.size()) {
            ::ChunkHeader ch;
            if (fread(&ch, sizeof(ch), 1, f) != 1 || (ch.raw_size == 0)) break;

            std::vector<char> comp(ch.comp_size);
            fread(comp.data(), 1, ch.comp_size, f);

            std::vector<char> decomp(ch.raw_size);
            size_t s_pos = 0, d_pos = 0;
            lzma_stream_buffer_decode(NULL, UINT64_MAX, NULL, (uint8_t*)comp.data(), &s_pos, ch.comp_size, (uint8_t*)decomp.data(), &d_pos, ch.raw_size);
            
            block.insert(block.end(), decomp.begin(), decomp.end());
        }

        const char* file_ptr = block.data() + block_cursor;
        uint64_t check_hash = XXH64(file_ptr, fe.meta.orig_size, 0);

        if (check_hash != fe.meta.xxhash) {
            fclose(f);
            return {false, "ERRORE INTEGRITÀ: " + fe.name};
        }

        if (!test_only) {
            IO::write_file_to_disk(fe.name, file_ptr, fe.meta.orig_size, fe.meta.timestamp);
        }

        block_cursor += fe.meta.orig_size;
        
        // Pulizia buffer per risparmiare RAM se abbiamo superato il cursore
        if (block_cursor > 256 * 1024 * 1024) {
             block.erase(block.begin(), block.begin() + block_cursor);
             block_cursor = 0;
        }
    }

    fclose(f);
    result.ok = true;
    return result;
}

TarcResult list(const std::string& arch_path) {
    TarcResult result;
    FILE* f = fopen(arch_path.c_str(), "rb");
    if (!f) return {false, "Errore apertura"};
    ::Header h;
    fread(&h, sizeof(h), 1, f);
    std::vector<::FileEntry> toc;
    IO::read_toc(f, h, toc);
    std::cout << "\n Analisi archivio: " << arch_path << "\n";
    for (const auto& fe : toc) UI::print_list_entry(fe.name, fe.meta.orig_size, fe.meta.is_duplicate ? 0 : fe.meta.orig_size, (Codec)fe.meta.codec);
    fclose(f);
    result.ok = true;
    return result;
}

TarcResult remove_files(const std::string& a, const std::vector<std::string>& p) { return {false, "Non supportato in Solid"}; }

} // namespace Engine
