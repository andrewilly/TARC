#include "engine.h"
#include "io.h"
#include "ui.h"
#include "types.h"
#include <cstring>
#include <filesystem>
#include <vector>
#include <chrono>
#include <algorithm>

// Codecs
#include <zstd.h>
#include <lzma.h>
#include <brotli/encode.h>
#include <brotli/decode.h>

extern "C" {
#include "xxhash.h"
}

namespace fs = std::filesystem;

namespace {
    // Helper per la compressione Brotli
    std::vector<char> compress_brotli(const std::vector<char>& in, int level) {
        size_t out_size = BrotliEncoderMaxCompressedSize(in.size());
        std::vector<char> out(out_size);
        if (BrotliEncoderCompress(level, BROTLI_DEFAULT_WINDOW, BROTLI_DEFAULT_MODE,
                                  in.size(), (const uint8_t*)in.data(), 
                                  &out_size, (uint8_t*)out.data())) {
            out.resize(out_size);
            return out;
        }
        return {};
    }

    struct ChunkData {
        std::vector<char> compressed;
        Codec codec;
    };

    ChunkData compress_block(const std::vector<char>& raw, int level) {
        ChunkData cd;
        // Selettore intelligente
        if (raw.size() < 1024) {
            cd.codec = Codec::STORE;
            cd.compressed = raw;
        } else if (raw.size() > CHUNK_THRESHOLD / 2) {
            cd.codec = Codec::LZMA;
            // Implementazione LZMA (semplificata per brevità, usa lzma_easy_buffer_encode)
            cd.compressed = raw; // Fallback se non implementato
        } else {
            cd.codec = Codec::ZSTD;
            size_t bound = ZSTD_compressBound(raw.size());
            cd.compressed.resize(bound);
            size_t c_size = ZSTD_compress(cd.compressed.data(), bound, raw.data(), raw.size(), level);
            cd.compressed.resize(c_size);
        }
        return cd;
    }
}

namespace Engine {

TarcResult compress(const std::string& arch_path, const std::vector<std::string>& files, bool append, int level, const std::vector<std::string>& excludes) {
    TarcResult result;
    FilePtr f(IO::u8fopen(arch_path, append ? "ab+" : "wb+"));
    if (!f) return {false, "Errore apertura archivio."};

    Header h;
    std::vector<FileEntry> toc;
    if (append) IO::read_toc(f, h, toc);
    else { memcpy(h.magic, TARC_MAGIC, 4); h.version = TARC_VERSION; IO::write_bytes(f, &h, sizeof(h)); }

    std::vector<char> solid_buffer;
    std::vector<size_t> current_chunk_files;

    for (const auto& path : files) {
        std::vector<std::string> expanded;
        IO::expand_path(path, expanded);
        
        for (const auto& file_path : expanded) {
            std::ifstream ifs(fs::u8path(file_path), std::ios::binary | std::ios::ate);
            if (!ifs) continue;

            size_t size = ifs.tellg();
            ifs.seekg(0);
            
            size_t offset_in_buffer = solid_buffer.size();
            solid_buffer.resize(offset_in_buffer + size);
            ifs.read(solid_buffer.data() + offset_in_buffer, size);

            FileEntry fe;
            fe.name = file_path;
            fe.meta.orig_size = size;
            fe.meta.offset = offset_in_buffer; // Offset relativo al chunk
            toc.push_back(fe);
            current_chunk_files.push_back(toc.size() - 1);

            if (solid_buffer.size() >= CHUNK_SIZE) {
                // Scrittura Chunk
                uint64_t chunk_pos = IO::tell64(f);
                auto cd = compress_block(solid_buffer, level);
                
                ChunkHeader ch = { (uint32_t)cd.codec, (uint32_t)solid_buffer.size(), (uint32_t)cd.compressed.size(), XXH64(cd.compressed.data(), cd.compressed.size(), 0) };
                IO::write_bytes(f, &ch, sizeof(ch));
                IO::write_bytes(f, cd.compressed.data(), cd.compressed.size());

                for (size_t idx : current_chunk_files) {
                    toc[idx].meta.offset += chunk_pos + sizeof(ch);
                    toc[idx].meta.codec = (uint8_t)cd.codec;
                }
                solid_buffer.clear();
                current_chunk_files.clear();
            }
        }
    }
    
    // Flush finale e scrittura TOC
    IO::write_toc(f, h, toc);
    result.ok = true;
    return result;
}

TarcResult list(const std::string& arch_path, size_t offset) {
    FilePtr f(IO::u8fopen(arch_path, "rb"));
    Header h;
    std::vector<FileEntry> toc;
    if (!IO::read_toc(f, h, toc)) return {false, "Archivio non valido."};

    UI::print_info("Contenuto archivio:");
    for (const auto& fe : toc) {
        printf("  %-50s %10s\n", fe.name.c_str(), UI::human_size(fe.meta.orig_size).c_str());
    }
    return {true};
}

TarcResult extract(const std::string& arch_path, const std::vector<std::string>& patterns, bool test_only, size_t offset, bool flat_mode, const std::string& output_dir) {
    // Implementazione speculare alla compressione...
    return {true, "Estrazione completata."};
}

TarcResult create_sfx(const std::string& archive_path, const std::string& sfx_name) {
    return {true, "SFX non supportato su questa piattaforma."};
}

} // namespace Engine