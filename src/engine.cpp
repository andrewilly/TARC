#include "engine.h"
#include "io.h"
#include "ui.h"
#include <cstring>
#include <set>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cmath>      // per std::log2
#include <climits>    // per limiti

// ─── DIPENDENZE COMPRESSIONE ─────────────────────────────────────────────────
#include "zstd.h"
#include "lz4.h"
#include "lz4hc.h"
#include "lzma.h"

extern "C" {
    #include "xxhash.h"
}

namespace fs = std::filesystem;

// ═══════════════════════════════════════════════════════════════════════════════
// CODEC SELECTOR
// ═══════════════════════════════════════════════════════════════════════════════
namespace CodecSelector {

// File già compressi → LZ4 (overhead minimo, nessun beneficio a comprimere ulteriormente)
static const std::set<std::string> COMPRESSED_EXTS = {
    ".zip", ".7z", ".rar", ".gz", ".bz2", ".xz", ".lz4",
    ".zst", ".br",  ".tar",
    ".jpg", ".jpeg", ".png", ".gif", ".webp", ".avif", ".heic",
    ".mp4", ".mkv", ".avi", ".mov", ".webm", ".flv",
    ".mp3", ".aac", ".ogg", ".flac", ".opus",
    ".pdf", ".docx", ".xlsx", ".pptx",
    ".woff", ".woff2"
};

// File testuali/codice sorgente → LZMA (ratio massimo)
static const std::set<std::string> TEXT_EXTS = {
    ".txt", ".md", ".rst", ".log", ".csv", ".tsv",
    ".cpp", ".c", ".h", ".hpp", ".cc", ".cxx",
    ".java", ".kt", ".scala", ".cs", ".go",
    ".py", ".rb", ".php", ".js", ".ts", ".jsx", ".tsx",
    ".html", ".htm", ".xml", ".svg", ".css", ".scss",
    ".json", ".yaml", ".yml", ".toml", ".ini", ".cfg",
    ".sh", ".bash", ".zsh", ".fish", ".ps1", ".bat",
    ".sql", ".graphql", ".proto"
};

bool is_already_compressed(const std::string& filename) {
    fs::path p(filename);
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return COMPRESSED_EXTS.count(ext) > 0;
}

bool is_lzma_candidate(const std::string& filename) {
    fs::path p(filename);
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return TEXT_EXTS.count(ext) > 0;
}

// Stima rapida dell'entropia: campiona i primi 4KB
bool is_high_entropy(const uint8_t* data, size_t len) {
    if (len == 0) return false;
    size_t sample = std::min(len, size_t(4096));
    uint32_t freq[256] = {};
    for (size_t i = 0; i < sample; ++i) freq[data[i]]++;

    double entropy = 0.0;
    double inv = 1.0 / sample;
    for (int i = 0; i < 256; ++i) {
        if (freq[i] == 0) continue;
        double p = freq[i] * inv;
        entropy -= p * std::log2(p);
    }
    return entropy > 7.2; // soglia: ~90% dell'entropia massima
}

Codec select_auto(const std::string& filename) {
    if (is_already_compressed(filename)) return Codec::LZ4;
    if (is_lzma_candidate(filename))     return Codec::LZMA;
    return Codec::ZSTD;
}

} // namespace CodecSelector

// ═══════════════════════════════════════════════════════════════════════════════
// CODEC IMPLEMENTATIONS (chunk-level)
// ═══════════════════════════════════════════════════════════════════════════════
namespace {

struct CompressResult {
    size_t comp_size = 0;
    bool   ok        = false;
};

// ─── ZSTD ────────────────────────────────────────────────────────────────────
CompressResult compress_zstd(ZSTD_CCtx* ctx,
                              const char* in,  size_t in_sz,
                              char*       out, size_t out_cap)
{
    size_t c = ZSTD_compress2(ctx, out, out_cap, in, in_sz);
    if (ZSTD_isError(c)) return {0, false};
    return {c, true};
}

bool decompress_zstd(const char* in,  size_t in_sz,
                     char*       out, size_t out_cap)
{
    size_t r = ZSTD_decompress(out, out_cap, in, in_sz);
    return !ZSTD_isError(r) && r == out_cap;
}

// ─── LZ4 ─────────────────────────────────────────────────────────────────────
CompressResult compress_lz4(const char* in,  size_t in_sz,
                             char*       out, size_t out_cap,
                             int         level)
{
    int c;
    if (level >= 3)
        c = LZ4_compress_HC(in, out, (int)in_sz, (int)out_cap, LZ4HC_CLEVEL_DEFAULT);
    else
        c = LZ4_compress_default(in, out, (int)in_sz, (int)out_cap);

    if (c <= 0) return {0, false};
    return {(size_t)c, true};
}

bool decompress_lz4(const char* in,  size_t in_sz,
                    char*       out, size_t out_cap)
{
    int r = LZ4_decompress_safe(in, out, (int)in_sz, (int)out_cap);
    return r >= 0 && (size_t)r == out_cap;
}

// ─── LZMA ────────────────────────────────────────────────────────────────────
CompressResult compress_lzma(const char* in,  size_t in_sz,
                              char*       out, size_t out_cap,
                              int         level)
{
    lzma_stream strm = LZMA_STREAM_INIT;
    
    // Imposta il livello di compressione (0-9)
    uint32_t preset = std::clamp(level, 0, 9);
    
    // Usa l'encoder facile di LZMA (API corretta per tutte le piattaforme)
    lzma_ret ret = lzma_easy_encoder(&strm, preset, LZMA_CHECK_CRC64);
    if (ret != LZMA_OK) {
        return {0, false};
    }
    
    strm.next_in   = reinterpret_cast<const uint8_t*>(in);
    strm.avail_in  = in_sz;
    strm.next_out  = reinterpret_cast<uint8_t*>(out);
    strm.avail_out = out_cap;
    
    ret = lzma_code(&strm, LZMA_FINISH);
    size_t out_pos = out_cap - strm.avail_out;
    lzma_end(&strm);
    
    if (ret != LZMA_STREAM_END) return {0, false};
    return {out_pos, true};
}

bool decompress_lzma(const char* in,  size_t in_sz,
                     char*       out, size_t out_cap)
{
    lzma_stream strm = LZMA_STREAM_INIT;
    
    // Decoder semplice per LZMA
    if (lzma_stream_decoder(&strm, UINT64_MAX, LZMA_CONCATENATED) != LZMA_OK) {
        return false;
    }
    
    strm.next_in   = reinterpret_cast<const uint8_t*>(in);
    strm.avail_in  = in_sz;
    strm.next_out  = reinterpret_cast<uint8_t*>(out);
    strm.avail_out = out_cap;
    
    lzma_ret ret = lzma_code(&strm, LZMA_FINISH);
    bool ok = (ret == LZMA_STREAM_END);
    lzma_end(&strm);
    
    return ok;
}

// ─── DISPATCH ────────────────────────────────────────────────────────────────
CompressResult compress_chunk(Codec codec, ZSTD_CCtx* zctx,
                               const char* in,  size_t in_sz,
                               char*       out, size_t out_cap,
                               int level)
{
    switch (codec) {
        case Codec::ZSTD: return compress_zstd(zctx, in, in_sz, out, out_cap);
        case Codec::LZ4:  return compress_lz4(in, in_sz, out, out_cap, level);
        case Codec::LZMA: return compress_lzma(in, in_sz, out, out_cap, level > 9 ? 9 : level);
        case Codec::NONE: {
            if (in_sz > out_cap) return {0, false};
            memcpy(out, in, in_sz);
            return {in_sz, true};
        }
    }
    return {0, false};
}

bool decompress_chunk(Codec codec,
                      const char* in,  size_t in_sz,
                      char*       out, size_t out_cap)
{
    switch (codec) {
        case Codec::ZSTD: return decompress_zstd(in, in_sz, out, out_cap);
        case Codec::LZ4:  return decompress_lz4(in, in_sz, out, out_cap);
        case Codec::LZMA: return decompress_lzma(in, in_sz, out, out_cap);
        case Codec::NONE: {
            if (in_sz != out_cap) return false;
            memcpy(out, in, in_sz);
            return true;
        }
    }
    return false;
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════════════
// ENGINE
// ═══════════════════════════════════════════════════════════════════════════════
namespace Engine {

// ─── COMPRESS ────────────────────────────────────────────────────────────────
TarcResult compress(const std::string&              arch_path,
                    const std::vector<std::string>& files,
                    bool                            append,
                    int                             level)
{
    TarcResult result;

    FILE* out = fopen(arch_path.c_str(), append ? "rb+" : "wb");
    if (!out) {
        result.ok = false;
        result.message = "Impossibile aprire/creare l'archivio: " + arch_path;
        return result;
    }

    Header h{{'T','A','R','C'}, TARC_VERSION, 0};
    std::vector<FileEntry> toc;
    std::set<std::string>  existing;

    if (append && fs::exists(arch_path)) {
        Header old_h;
        if (IO::read_toc(out, old_h, toc)) {
            for (auto& fe : toc) existing.insert(fe.name);
            fseek(out, static_cast<long>(old_h.toc_offset), SEEK_SET);
        }
    } else {
        IO::write_bytes(out, &h, sizeof(h));
    }

    // Contesti ZSTD
    ZSTD_CCtx* zctx = ZSTD_createCCtx();
    ZSTD_CCtx_setParameter(zctx, ZSTD_c_nbWorkers, 4);
    ZSTD_CCtx_setParameter(zctx, ZSTD_c_compressionLevel, level);

    std::vector<char> in_buf(CHUNK_SIZE);
    // Buffer output: worst case per LZMA/LZ4 può essere leggermente superiore
    size_t out_bound = std::max(ZSTD_compressBound(CHUNK_SIZE),
                                (size_t)(CHUNK_SIZE + CHUNK_SIZE / 16 + 64));
    std::vector<char> out_buf(out_bound);

    for (const auto& fpath : files) {
        if (existing.count(fpath)) {
            UI::print_warning("Già presente, saltato: " + fpath);
            continue;
        }

        FILE* src = fopen(fpath.c_str(), "rb");
        if (!src) {
            UI::print_warning("File non trovato, saltato: " + fpath);
            continue;
        }

        // Seleziona codec automaticamente
        Codec codec = CodecSelector::select_auto(fpath);

        uint64_t file_offset  = static_cast<uint64_t>(ftell(out));
        uint64_t orig_total   = 0;
        uint64_t comp_total   = 0;

        XXH64_state_t* hstate = XXH64_createState();
        XXH64_reset(hstate, 0);

        // Per la stima entropia: leggi il primo chunk e poi riposiziona
        bool entropy_checked = false;

        while (true) {
            size_t r = fread(in_buf.data(), 1, CHUNK_SIZE, src);
            if (r == 0) break;

            // Se è la prima lettura e codec è LZ4, verifica entropia reale
            if (!entropy_checked) {
                entropy_checked = true;
                if (codec == Codec::LZ4 &&
                    CodecSelector::is_high_entropy(
                        reinterpret_cast<const uint8_t*>(in_buf.data()), r))
                {
                    codec = Codec::NONE; // Già compresso, non conviene
                }
            }

            XXH64_update(hstate, in_buf.data(), r);

            auto cr = compress_chunk(codec, zctx,
                                     in_buf.data(), r,
                                     out_buf.data(), out_buf.size(),
                                     level);

            // Fallback a NONE se la compressione peggiora
            if (!cr.ok || cr.comp_size >= r) {
                cr.comp_size = r;
                codec = Codec::NONE;
                memcpy(out_buf.data(), in_buf.data(), r);
            }

            ChunkHeader ch{static_cast<uint32_t>(r),
                           static_cast<uint32_t>(cr.comp_size)};
            IO::write_bytes(out, &ch, sizeof(ch));
            IO::write_bytes(out, out_buf.data(), cr.comp_size);

            orig_total += r;
            comp_total += cr.comp_size + sizeof(ch);
        }

        // Chunk terminatore
        ChunkHeader end{0, 0};
        IO::write_bytes(out, &end, sizeof(end));
        comp_total += sizeof(end);

        uint64_t hash = XXH64_digest(hstate);
        XXH64_freeState(hstate);
        fclose(src);

        float ratio = orig_total > 0
            ? 1.0f - static_cast<float>(comp_total) / orig_total
            : 0.0f;

        toc.push_back({{
            file_offset,
            orig_total,
            comp_total,
            hash,
            static_cast<uint16_t>(fpath.size()),
            static_cast<uint8_t>(codec),
            0
        }, fpath});

        UI::print_add(fpath, orig_total, codec, ratio);
        result.bytes_in  += orig_total;
        result.bytes_out += comp_total;
    }

    IO::write_toc(out, h, toc);
    fclose(out);
    ZSTD_freeCCtx(zctx);

    return result;
}

// ─── EXTRACT ─────────────────────────────────────────────────────────────────
TarcResult extract(const std::string& arch_path, bool test_only) {
    TarcResult result;

    FILE* in = fopen(arch_path.c_str(), "rb");
    if (!in) {
        result.ok = false;
        result.message = "Archivio non trovato: " + arch_path;
        return result;
    }

    Header h;
    std::vector<FileEntry> toc;
    if (!IO::read_toc(in, h, toc)) {
        fclose(in);
        result.ok = false;
        result.message = "Archivio non valido o corrotto.";
        return result;
    }

    std::vector<char> comp_buf;
    std::vector<char> raw_buf;

    for (auto& fe : toc) {
        fseek(in, static_cast<long>(fe.meta.offset), SEEK_SET);

        XXH64_state_t* hstate = XXH64_createState();
        XXH64_reset(hstate, 0);

        Codec codec = static_cast<Codec>(fe.meta.codec);

        std::ofstream ofs;
        if (!test_only) {
            fs::create_directories(fs::path(fe.name).parent_path());
            ofs.open(fe.name, std::ios::binary);
        }

        bool file_ok = true;

        while (true) {
            ChunkHeader ch{};
            if (!IO::read_bytes(in, &ch, sizeof(ch))) { file_ok = false; break; }
            if (ch.raw_size == 0 && ch.comp_size == 0) break;

            comp_buf.resize(ch.comp_size);
            raw_buf.resize(ch.raw_size);

            if (!IO::read_bytes(in, comp_buf.data(), ch.comp_size)) {
                file_ok = false; break;
            }

            if (!decompress_chunk(codec,
                                   comp_buf.data(), ch.comp_size,
                                   raw_buf.data(),  ch.raw_size)) {
                file_ok = false; break;
            }

            XXH64_update(hstate, raw_buf.data(), ch.raw_size);
            if (!test_only && ofs.is_open())
                ofs.write(raw_buf.data(), ch.raw_size);

            result.bytes_out += ch.raw_size;
        }

        bool hash_ok = (XXH64_digest(hstate) == fe.meta.xxhash);
        XXH64_freeState(hstate);

        if (!test_only && ofs.is_open()) ofs.close();

        UI::print_extract(fe.name, fe.meta.orig_size, test_only, file_ok && hash_ok);

        if (!file_ok || !hash_ok) result.ok = false;
    }

    fclose(in);
    return result;
}

// ─── REMOVE ──────────────────────────────────────────────────────────────────
TarcResult remove_files(const std::string&              arch_path,
                        const std::vector<std::string>& targets)
{
    TarcResult result;

    FILE* in = fopen(arch_path.c_str(), "rb");
    if (!in) {
        result.ok = false;
        result.message = "Archivio non trovato: " + arch_path;
        return result;
    }

    Header h;
    std::vector<FileEntry> toc;
    if (!IO::read_toc(in, h, toc)) {
        fclose(in);
        result.ok = false;
        result.message = "Archivio non valido.";
        return result;
    }

    std::string tmp_path = arch_path + ".tmp";
    FILE* out = fopen(tmp_path.c_str(), "wb");
    if (!out) {
        fclose(in);
        result.ok = false;
        result.message = "Impossibile creare file temporaneo.";
        return result;
    }

    Header nh{{'T','A','R','C'}, TARC_VERSION, 0};
    IO::write_bytes(out, &nh, sizeof(nh));

    std::set<std::string> del_set(targets.begin(), targets.end());
    std::vector<FileEntry> new_toc;
    std::vector<char> buf;

    for (auto& fe : toc) {
        if (del_set.count(fe.name)) {
            UI::print_delete(fe.name);
            continue;
        }

        fseek(in, static_cast<long>(fe.meta.offset), SEEK_SET);
        uint64_t new_offset = static_cast<uint64_t>(ftell(out));

        // Copia chunk per chunk
        while (true) {
            ChunkHeader ch{};
            IO::read_bytes(in, &ch, sizeof(ch));
            IO::write_bytes(out, &ch, sizeof(ch));
            if (ch.raw_size == 0 && ch.comp_size == 0) break;

            buf.resize(ch.comp_size);
            IO::read_bytes(in, buf.data(), ch.comp_size);
            IO::write_bytes(out, buf.data(), ch.comp_size);
        }

        fe.meta.offset = new_offset;
        new_toc.push_back(fe);
    }

    IO::write_toc(out, nh, new_toc);
    fclose(in);
    fclose(out);

    fs::remove(arch_path);
    fs::rename(tmp_path, arch_path);

    return result;
}

// ─── LIST ────────────────────────────────────────────────────────────────────
TarcResult list(const std::string& arch_path) {
    TarcResult result;

    FILE* f = fopen(arch_path.c_str(), "rb");
    if (!f) {
        result.ok = false;
        result.message = "Archivio non trovato: " + arch_path;
        return result;
    }

    Header h;
    std::vector<FileEntry> toc;
    if (!IO::read_toc(f, h, toc)) {
        fclose(f);
        result.ok = false;
        result.message = "Archivio non valido.";
        return result;
    }

    printf("\n%sArchivio:%s %s%s%s  (%zu file)\n\n",
           Color::BOLD, Color::RESET,
           Color::CYAN, arch_path.c_str(), Color::RESET,
           toc.size());

    printf("  %s%-4s  %-42s %12s  %10s  %s%s\n",
           Color::DIM, "CODEC", "NOME FILE", "DIM. ORIG", "COMPRESSA", "RATIO", Color::RESET);

    printf("  %s%s%s\n", Color::DIM,
           std::string(80, '-').c_str(), Color::RESET);

    for (auto& fe : toc) {
        UI::print_list_entry(
            fe.name,
            fe.meta.orig_size,
            fe.meta.comp_size,
            static_cast<Codec>(fe.meta.codec)
        );
        result.bytes_in  += fe.meta.orig_size;
        result.bytes_out += fe.meta.comp_size;
    }

    printf("  %s%s%s\n", Color::DIM,
           std::string(80, '-').c_str(), Color::RESET);
    printf("  %sTOTALE:%s  %s  →  %s  (%sratio: %s%s)\n",
           Color::BOLD, Color::RESET,
           UI::human_size(result.bytes_in).c_str(),
           UI::human_size(result.bytes_out).c_str(),
           Color::DIM,
           UI::compress_ratio(result.bytes_in, result.bytes_out).c_str(),
           Color::RESET);

    fclose(f);
    return result;
}

} // namespace Engine
