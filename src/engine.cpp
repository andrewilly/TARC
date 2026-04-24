#include "engine.h"
#include "io.h"
#include "ui.h"
#include "types.h"
#include <cstring>
#include <climits>
#include <map>
#include <filesystem>
#include <fstream>
#include <vector>
#include <iostream>
#include <chrono>
#include <future>
#include <algorithm>
#include <set>
#include <unordered_set>

// ─── INCLUDE CODEC ─────────────────────────────────────────────────────────
#include <zstd.h>
#include <lz4.h>
#include <lzma.h>
#include <brotli/encode.h>
#include <brotli/decode.h>

extern "C" {
    #include "xxhash.h"
}

namespace fs = std::filesystem;

// ═══════════════════════════════════════════════════════════════════════════════
// NAMESPACE ANONIMO — Helper interni, non visibili all'linker
// ═══════════════════════════════════════════════════════════════════════════════
namespace {

// ─── NORMALIZZAZIONE PERCORSI ────────────────────────────────────────────────
std::string normalize_path(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}

// ─── GLOB MATCHING ROBUSTO ───────────────────────────────────────────────────
// Supporta *, ? e combinazioni (*.txt, *foo*, prefix*suffix, ecc.)
// Implementazione iterativa standard con backtracking sul wildcards.
bool glob_match(const std::string& text, const std::string& pattern) {
    if (pattern.empty()) return true;

    size_t t = 0, p = 0;
    size_t star_pos = std::string::npos;
    size_t match_pos = 0;

    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
            t++; p++;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star_pos = p++;
            match_pos = t;
        } else if (star_pos != std::string::npos) {
            p = star_pos + 1;
            t = ++match_pos;
        } else {
            return false;
        }
    }

    while (p < pattern.size() && pattern[p] == '*') p++;
    return p == pattern.size();
}

// ─── RESOLVE WILDCARDS UNICODE-AWARE ─────────────────────────────────────────
// Usa fs::u8path + fs::directory_iterator per supporto Unicode cross-platform.
void resolve_wildcards(const std::string& pattern, std::vector<std::string>& out) {
    if (pattern.find('*') != std::string::npos || pattern.find('?') != std::string::npos) {
        fs::path p = fs::u8path(pattern);
        fs::path dir = p.parent_path();
        std::string filter = p.filename().string();

        if (dir.empty()) dir = fs::current_path();

        std::error_code ec;
        if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) return;

        for (auto& entry : fs::directory_iterator(dir, ec)) {
            std::string name = entry.path().filename().string();
            if (name == "." || name == "..") continue;

            std::string full = normalize_path(entry.path().string());

            if (entry.is_regular_file(ec)) {
                if (glob_match(name, filter)) {
                    out.push_back(full);
                }
            } else if (entry.is_directory(ec)) {
                for (auto& sub : fs::recursive_directory_iterator(entry.path(), ec))
                    if (sub.is_regular_file(ec))
                        out.push_back(normalize_path(sub.path().string()));
            }
        }
    } else {
        fs::path p = fs::u8path(pattern);
        std::error_code ec;
        if (fs::exists(p, ec)) {
            if (fs::is_directory(p, ec)) {
                for (auto& entry : fs::recursive_directory_iterator(p, ec))
                    if (entry.is_regular_file(ec))
                        out.push_back(normalize_path(entry.path().string()));
            } else {
                out.push_back(normalize_path(p.string()));
            }
        }
    }
}

// ─── PATTERN MATCHING PER FILTRI ─────────────────────────────────────────────
// Determina se un percorso corrisponde a un pattern di filtro.
// Se il pattern non contiene separatori, confronta solo col filename.
bool match_pattern(const std::string& full_path, const std::string& pattern) {
    if (pattern.empty()) return true;

    std::string target = full_path;
    if (pattern.find('/') == std::string::npos && pattern.find('\\') == std::string::npos) {
        target = fs::path(full_path).filename().string();
    }

    return glob_match(target, pattern);
}

bool is_excluded(const std::string& path, const std::vector<std::string>& exclude_patterns) {
    for (const auto& pat : exclude_patterns) {
        if (match_pattern(path, pat)) return true;
    }
    return false;
}

// ─── ESTENSIONI INCOMPRESSIBILI ──────────────────────────────────────────────
// Usa unordered_set per lookup O(1) medio.
const std::unordered_set<std::string>& incompressible_extensions() {
    static const std::unordered_set<std::string> skip = {
        ".zip", ".7z", ".rar", ".gz", ".bz2", ".xz", ".zst", ".lz4",
        ".br", ".tar", ".tgz", ".tbz2", ".txz", ".cab", ".arj",
        ".jpg", ".jpeg", ".png", ".gif", ".webp", ".ico", ".heic",
        ".heif", ".avif", ".jxl",
        ".mp3", ".mp4", ".ogg", ".flac", ".aac", ".wma", ".wmv",
        ".avi", ".mkv", ".mov", ".webm", ".opus", ".m4a", ".m4v",
        ".pdf", ".docx", ".xlsx", ".pptx", ".odt", ".ods", ".odp",
        ".epub", ".xps",
        ".woff", ".woff2", ".ttf", ".otf", ".eot",
        ".exe", ".dll", ".so", ".dylib", ".nupkg", ".jar", ".apk",
        ".msi", ".crx",
        ".ktx", ".ktx2", ".basis", ".dds", ".crn"
    };
    return skip;
}

// ─── HELPER: RISOLUZIONE NOME FLAT ───────────────────────────────────────────
// Gestisce rinomina automatica in caso di conflitto in modalita' flat.
std::string resolve_flat_name(const std::string& name, std::map<std::string, int>& counter) {
    fs::path p(name);
    std::string filename = p.filename().string();
    auto it = counter.find(filename);
    if (it != counter.end()) {
        it->second++;
        int idx = it->second;
        size_t dot_pos = filename.find_last_of('.');
        if (dot_pos != std::string::npos) {
            filename = filename.substr(0, dot_pos) + "_" + std::to_string(idx) + filename.substr(dot_pos);
        } else {
            filename += "_" + std::to_string(idx);
        }
    } else {
        counter[filename] = 0;
    }
    return filename;
}

// ─── HELPER: COSTRUZIONE PERCORSO DI ESTRAZIONE ──────────────────────────────
// Combina flat_mode, output_dir e sanitizzazione in un'unica funzione.
// Ritorna stringa vuota se il percorso e' pericoloso.
std::string build_extract_path(const std::string& name, bool flat_mode,
                                const std::string& output_dir,
                                std::map<std::string, int>& flat_counter,
                                bool is_test_only) {
    std::string final_path = name;

    if (flat_mode) {
        final_path = resolve_flat_name(name, flat_counter);
    }

    if (!is_test_only && !output_dir.empty()) {
        final_path = output_dir + "/" + final_path;
    }

    if (!is_test_only) {
        std::string safe_path = IO::sanitize_path(final_path);
        if (safe_path.empty()) return "";
        final_path = safe_path;
    }

    return final_path;
}

// ═══════════════════════════════════════════════════════════════════════════════
// COMPRESS WORKER MULTI-CODEC
// ═══════════════════════════════════════════════════════════════════════════════

struct ChunkResult {
    std::vector<char> compressed_data;
    uint64_t raw_size;       // uint64_t per supportare chunk > 4 GB
    Codec codec;
    bool success;
    std::string error_msg;   // Accumula errori invece di stampare dal thread
};

// ─── HELPER: fallback a STORE ────────────────────────────────────────────────
static void fallback_to_store(ChunkResult& res, std::vector<char>& raw_data) {
    res.compressed_data = std::move(raw_data);
    res.codec = Codec::STORE;
    res.success = true;
}

ChunkResult compress_worker(std::vector<char> raw_data, int level, Codec chosen_codec) {
    ChunkResult res;
    res.raw_size = raw_data.size();
    res.codec = chosen_codec;
    res.success = false;

    if (raw_data.empty()) {
        res.success = true;
        return res;
    }

    if (chosen_codec == Codec::STORE) {
        res.compressed_data = std::move(raw_data);
        res.success = true;
        return res;
    }

    // ── ZSTD ─────────────────────────────────────────────────────────────────
    if (chosen_codec == Codec::ZSTD) {
        size_t max_out = ZSTD_compressBound(raw_data.size());
        res.compressed_data.resize(max_out);
        int zstd_level = std::clamp(level, 1, 19);
        size_t result = ZSTD_compress(
            res.compressed_data.data(), max_out,
            raw_data.data(), raw_data.size(), zstd_level);
        if (!ZSTD_isError(result)) {
            res.compressed_data.resize(result);
            if (res.compressed_data.size() >= raw_data.size()) {
                fallback_to_store(res, raw_data);
            }
            res.success = true;
        } else {
            res.error_msg = "ZSTD compression failed, falling back to STORE";
            fallback_to_store(res, raw_data);
        }
        return res;
    }

    // ── LZ4 ──────────────────────────────────────────────────────────────────
    if (chosen_codec == Codec::LZ4) {
        // LZ4_compressBound usa int, overflow per dati > 2 GB
        if (raw_data.size() > static_cast<size_t>(INT_MAX)) {
            res.error_msg = "LZ4: data too large (>2 GB), falling back to STORE";
            fallback_to_store(res, raw_data);
            return res;
        }
        int src_size = static_cast<int>(raw_data.size());
        int max_out = LZ4_compressBound(src_size);
        if (max_out <= 0) {
            res.error_msg = "LZ4_compressBound failed, falling back to STORE";
            fallback_to_store(res, raw_data);
            return res;
        }
        res.compressed_data.resize(static_cast<size_t>(max_out));
        int result = LZ4_compress_default(
            raw_data.data(), res.compressed_data.data(),
            src_size, max_out);
        if (result > 0) {
            res.compressed_data.resize(static_cast<size_t>(result));
            if (res.compressed_data.size() >= raw_data.size()) {
                fallback_to_store(res, raw_data);
            }
            res.success = true;
        } else {
            res.error_msg = "LZ4 compression failed, falling back to STORE";
            fallback_to_store(res, raw_data);
        }
        return res;
    }

    // ── BROTLI ───────────────────────────────────────────────────────────────
    if (chosen_codec == Codec::BR) {
        size_t max_out = BrotliEncoderMaxCompressedSize(raw_data.size());
        if (max_out == 0) {
            res.error_msg = "Brotli: input too large for encoder, falling back to STORE";
            fallback_to_store(res, raw_data);
            return res;
        }
        res.compressed_data.resize(max_out);
        int brotli_level = std::clamp(level, 0, 11);
        size_t encoded_size = max_out;
        BROTLI_BOOL ok = BrotliEncoderCompress(
            brotli_level, BROTLI_DEFAULT_WINDOW, BROTLI_DEFAULT_MODE,
            raw_data.size(), reinterpret_cast<const uint8_t*>(raw_data.data()),
            &encoded_size, reinterpret_cast<uint8_t*>(res.compressed_data.data()));
        if (ok == BROTLI_TRUE) {
            res.compressed_data.resize(encoded_size);
            if (res.compressed_data.size() >= raw_data.size()) {
                fallback_to_store(res, raw_data);
            }
            res.success = true;
        } else {
            res.error_msg = "Brotli compression failed, falling back to STORE";
            fallback_to_store(res, raw_data);
        }
        return res;
    }

    // ── LZMA ─────────────────────────────────────────────────────────────────
    if (chosen_codec == Codec::LZMA) {
        size_t max_out = lzma_stream_buffer_bound(raw_data.size());
        res.compressed_data.resize(max_out);
        size_t out_pos = 0;
        uint32_t preset = (level < 0) ? 9 : static_cast<uint32_t>(level);
        // LZMA_PRESET_EXTREME solo per livelli alti (>=8), altrimenti
        // la compressione e' 5-10x piu lenta senza guadagno significativo
        if (preset >= 8) preset |= LZMA_PRESET_EXTREME;
        lzma_ret ret = lzma_easy_buffer_encode(
            preset, LZMA_CHECK_CRC64, NULL,
            reinterpret_cast<const uint8_t*>(raw_data.data()), raw_data.size(),
            reinterpret_cast<uint8_t*>(res.compressed_data.data()), &out_pos, max_out);
        if (ret == LZMA_OK) {
            res.compressed_data.resize(out_pos);
            if (res.compressed_data.size() >= raw_data.size()) {
                fallback_to_store(res, raw_data);
            }
            res.success = true;
        } else {
            res.error_msg = "LZMA compression failed (ret=" + std::to_string(ret) + "), falling back to STORE";
            fallback_to_store(res, raw_data);
        }
        return res;
    }

    // Codec sconosciuto: fallback a STORE
    fallback_to_store(res, raw_data);
    return res;
}

// ═══════════════════════════════════════════════════════════════════════════════
// DECOMPRESS HELPER — Multi-codec
// ═══════════════════════════════════════════════════════════════════════════════

bool decompress_chunk(const std::vector<char>& comp_data, uint32_t codec,
                      std::vector<char>& out_data, size_t raw_size) {
    // Rifiuta decompressione con raw_size == 0 per codec diversi da STORE
    if (raw_size == 0 && codec != static_cast<uint32_t>(Codec::STORE)) return false;

    out_data.resize(raw_size);

    if (codec == static_cast<uint32_t>(Codec::STORE)) {
        if (raw_size > comp_data.size()) return false;
        memcpy(out_data.data(), comp_data.data(), raw_size);
        return true;
    }
    if (codec == static_cast<uint32_t>(Codec::ZSTD)) {
        size_t result = ZSTD_decompress(out_data.data(), raw_size, comp_data.data(), comp_data.size());
        return !ZSTD_isError(result);
    }
    if (codec == static_cast<uint32_t>(Codec::LZ4)) {
        // Validazione dimensione per LZ4 (limitato a INT_MAX)
        if (raw_size > static_cast<size_t>(INT_MAX) || comp_data.size() > static_cast<size_t>(INT_MAX))
            return false;
        int result = LZ4_decompress_safe(comp_data.data(), out_data.data(),
            static_cast<int>(comp_data.size()), static_cast<int>(raw_size));
        return result > 0;
    }
    if (codec == static_cast<uint32_t>(Codec::BR)) {
        size_t decoded_size = raw_size;
        BrotliDecoderResult result = BrotliDecoderDecompress(
            comp_data.size(), reinterpret_cast<const uint8_t*>(comp_data.data()),
            &decoded_size, reinterpret_cast<uint8_t*>(out_data.data()));
        return result == BROTLI_DECODER_RESULT_SUCCESS && decoded_size == raw_size;
    }
    if (codec == static_cast<uint32_t>(Codec::LZMA)) {
        size_t src_p = 0, dst_p = 0;
        uint64_t limit = UINT64_MAX;
        lzma_ret ret = lzma_stream_buffer_decode(&limit, 0, NULL,
            reinterpret_cast<const uint8_t*>(comp_data.data()), &src_p, comp_data.size(),
            reinterpret_cast<uint8_t*>(out_data.data()), &dst_p, raw_size);
        return (ret == LZMA_OK || ret == LZMA_STREAM_END);
    }
    return false;
}

// ═══════════════════════════════════════════════════════════════════════════════
// READ NEXT CHUNK — Con verifica checksum e validazione dimensione
// ═══════════════════════════════════════════════════════════════════════════════

struct DecodedChunk {
    std::vector<char> data;
    bool valid = false;
};

DecodedChunk read_next_chunk(FILE* f) {
    DecodedChunk result;
    ::ChunkHeader ch;

    if (fread(&ch, sizeof(ch), 1, f) != 1 || ch.raw_size == 0) return result;

    // Protezione OOM: limita la dimensione dei dati compressi letti
    if (ch.comp_size > MAX_CHUNK_COMP_SIZE) {
        UI::print_error("Chunk compresso troppo grande (" + std::to_string(ch.comp_size) +
                        " bytes). Archivio corrotto o malevolo.");
        return result;
    }

    std::vector<char> comp(ch.comp_size);
    if (fread(comp.data(), 1, ch.comp_size, f) != ch.comp_size) return result;

    if (ch.checksum != 0) {
        XXH64_hash_t computed = XXH64(comp.data(), ch.comp_size, 0);
        if (computed != ch.checksum) {
            UI::print_error("Checksum chunk non valido! Archivio corrotto.");
            return result;
        }
    }

    if (!decompress_chunk(comp, ch.codec, result.data, ch.raw_size)) {
        UI::print_error("Errore decompressione chunk (codec: " + std::to_string(ch.codec) + ")");
        return result;
    }

    result.valid = true;
    return result;
}

// ─── STRUCT PER TASK DI ESTRAZIONE ASINCRONA ─────────────────────────────────
struct ExtractTask {
    size_t toc_index;
    std::string final_path;
    std::vector<char> data;
    uint64_t timestamp;
    bool is_duplicate;
    uint32_t duplicate_of_idx;
    uint64_t xxhash;
    uint64_t orig_size;
};

} // namespace anonimo

// ═══════════════════════════════════════════════════════════════════════════════
// CODEC SELECTOR
// ═══════════════════════════════════════════════════════════════════════════════

namespace CodecSelector {
    bool is_compressibile(const std::string& ext) {
        std::string e = ext;
        std::transform(e.begin(), e.end(), e.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return incompressible_extensions().find(e) == incompressible_extensions().end();
    }

    Codec select(const std::string& path, size_t size, int level) {
        if (!is_compressibile(fs::path(path).extension().string())) return Codec::STORE;
        if (level <= 2) return Codec::LZ4;
        if (level <= 5) return (size < CODEC_SWITCH_SIZE) ? Codec::ZSTD : Codec::LZMA;
        return Codec::LZMA;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// ENGINE — Implementazione principale v2.04
// ═══════════════════════════════════════════════════════════════════════════════

namespace Engine {

// ═══════════════════════════════════════════════════════════════════════════════
// CREATE SFX — Generazione archivio autoestraente
// ═══════════════════════════════════════════════════════════════════════════════

TarcResult create_sfx(const std::string& archive_path, const std::string& sfx_name) {
    // Cerca lo stub SFX nella directory corrente e in quella dell'eseguibile
    std::string stub_path = "tarc_sfx_stub.exe";
    if (!fs::exists(fs::u8path(stub_path))) {
        std::string exe_dir = IO::get_exe_directory();
        std::string alt_stub = exe_dir + "/tarc_sfx_stub.exe";
        if (fs::exists(fs::u8path(alt_stub))) {
            stub_path = alt_stub;
        } else {
            return {false, "Stub SFX non trovato (cercato in . e " + exe_dir + ")."};
        }
    }

#ifdef _WIN32
    std::ifstream stub_in(fs::u8path(stub_path), std::ios::binary);
    std::ifstream data_in(fs::u8path(archive_path), std::ios::binary);
    std::ofstream sfx_out(fs::u8path(sfx_name), std::ios::binary);
#else
    std::ifstream stub_in(stub_path, std::ios::binary);
    std::ifstream data_in(archive_path, std::ios::binary);
    std::ofstream sfx_out(sfx_name, std::ios::binary);
#endif

    if (!stub_in || !data_in || !sfx_out) return {false, "Errore fatale durante la fusione SFX."};

    // 1. Scrivi lo stub
    sfx_out << stub_in.rdbuf();
    stub_in.close();

    // 2. Registra l'offset dove inizia l'archivio
    auto archive_offset = static_cast<uint64_t>(sfx_out.tellp());

    // 3. Scrivi l'archivio
    sfx_out << data_in.rdbuf();
    data_in.close();

    // 4. Scrivi il trailer SFX con l'offset esatto
    SFXTrailer trailer;
    trailer.archive_offset = archive_offset;
    memcpy(trailer.magic, SFX_TRAILER_MAGIC, 4);
    sfx_out.write(reinterpret_cast<const char*>(&trailer), sizeof(trailer));

    if (!sfx_out.good()) return {false, "Errore scrittura trailer SFX."};

    return {true, "Archivio autoestraente generato."};
}

// ═══════════════════════════════════════════════════════════════════════════════
// COMPRESS — v2.04
// ═══════════════════════════════════════════════════════════════════════════════

TarcResult compress(const std::string& archive_path, const std::vector<std::string>& inputs,
                    bool append, int level, const std::vector<std::string>& exclude_patterns) {
    TarcResult result;
    auto t_start = std::chrono::steady_clock::now();

    std::vector<std::string> expanded_files;
    for (const auto& in : inputs) resolve_wildcards(in, expanded_files);

    if (!exclude_patterns.empty()) {
        size_t before = expanded_files.size();
        expanded_files.erase(
            std::remove_if(expanded_files.begin(), expanded_files.end(),
                [&](const std::string& p) { return is_excluded(p, exclude_patterns); }),
            expanded_files.end());
        result.skip_count += static_cast<uint32_t>(before - expanded_files.size());
        if (before != expanded_files.size()) {
            UI::print_verbose("Exclude: " + std::to_string(before - expanded_files.size()) +
                              " file esclusi su " + std::to_string(before));
        }
    }

    if (expanded_files.empty()) return {false, "Nessun file trovato."};

    std::vector<::FileEntry> final_toc;
    std::map<uint64_t, uint32_t> hash_map;
    ::Header h{};

    // ── APPEND: leggi e valida TOC esistente ─────────────────────────────────
    if (append && fs::exists(fs::u8path(archive_path))) {
        FilePtr f_old(IO::u8fopen(archive_path, "rb"));
        if (!f_old) return {false, "Impossibile aprire l'archivio per lettura."};
        if (fread(&h, sizeof(h), 1, f_old) != 1) return {false, "Header archivio corrotto."};

        if (!IO::validate_header(h)) {
            return {false, "Il file non e' un archivio TARC valido o versione incompatibile."};
        }

        if (!IO::read_toc(f_old, h, final_toc)) {
            return {false, "Impossibile leggere TOC dall'archivio."};
        }
        for (size_t k = 0; k < final_toc.size(); ++k) {
            if (!final_toc[k].meta.is_duplicate) hash_map[final_toc[k].meta.xxhash] = static_cast<uint32_t>(k);
        }
    } else {
        memcpy(h.magic, TARC_MAGIC, 4);
        h.version = TARC_VERSION;
    }

    // ── SCRITTURA ATOMICA ─────────────────────────────────────────────────────
    std::string temp_path = IO::make_temp_path(archive_path);
    std::string actual_write_path = temp_path;

    if (append) {
        try {
            fs::copy_file(fs::u8path(archive_path), fs::u8path(temp_path), fs::copy_options::overwrite_existing);
        } catch (...) {
            return {false, "Impossibile creare file temporaneo per append atomico."};
        }
    }

    FilePtr f(IO::u8fopen(actual_write_path, append ? "rb+" : "wb"));
    if (!f) {
        IO::safe_remove(temp_path);
        return {false, "ERRORE CRITICO: Impossibile scrivere l'archivio."};
    }

    if (append) {
        if (!IO::seek64(f, static_cast<int64_t>(h.toc_offset), SEEK_SET)) {
            IO::safe_remove(temp_path);
            return {false, "Errore seek nell'archivio."};
        }
    } else {
        if (fwrite(&h, sizeof(h), 1, f) != 1) {
            IO::safe_remove(temp_path);
            return {false, "Errore scrittura header."};
        }
    }

    // ── SOLID BUFFER E GESTIONE CHUNK ─────────────────────────────────────────
    std::vector<char> solid_buf;
    solid_buf.reserve(CHUNK_THRESHOLD);

    std::future<ChunkResult> future_chunk;
    bool worker_active = false;
    Codec last_codec = Codec::LZMA;

    auto write_chunk_result = [&](ChunkResult& res) -> bool {
        if (!res.success) return false;
        // I chunk non devono superare i 4 GB (limite imposto da ChunkHeader uint32_t)
        if (res.raw_size > UINT32_MAX || res.compressed_data.size() > UINT32_MAX) {
            UI::print_error("Chunk troppo grande (> 4 GB). Ridurre CHUNK_THRESHOLD.");
            return false;
        }
        ::ChunkHeader ch = { static_cast<uint32_t>(res.codec), static_cast<uint32_t>(res.raw_size),
                              static_cast<uint32_t>(res.compressed_data.size()), 0 };
        ch.checksum = XXH64(res.compressed_data.data(), res.compressed_data.size(), 0);
        if (fwrite(&ch, sizeof(ch), 1, f) != 1) return false;
        if (fwrite(res.compressed_data.data(), 1, res.compressed_data.size(), f) != res.compressed_data.size())
            return false;
        result.bytes_out += res.compressed_data.size();
        result.codec_bytes[res.codec] += res.compressed_data.size();
        result.codec_chunks[res.codec]++;
        return true;
    };

    auto write_pending_async = [&]() -> bool {
        if (!worker_active) return true;
        ChunkResult res = future_chunk.get();
        worker_active = false;
        // Stampa errori accumulati dal worker thread
        if (!res.error_msg.empty()) {
            UI::print_warning(res.error_msg);
        }
        return write_chunk_result(res);
    };

    auto flush_solid_buf = [&](Codec codec) -> bool {
        if (solid_buf.empty()) return true;
        if (!write_pending_async()) return false;
        future_chunk = std::async(std::launch::async, compress_worker,
                                   std::move(solid_buf), level, codec);
        worker_active = true;
        solid_buf.clear();
        solid_buf.reserve(CHUNK_THRESHOLD);
        return true;
    };

    // ── LOOP PRINCIPALE SU TUTTI I FILE ───────────────────────────────────────
    for (size_t i = 0; i < expanded_files.size(); ++i) {
        const std::string& disk_path = expanded_files[i];
        UI::print_progress(i + 1, expanded_files.size(), fs::path(disk_path).filename().string());

        std::error_code ec;
        fs::path disk_p = fs::u8path(disk_path);
        if (!fs::exists(disk_p, ec)) continue;
        uintmax_t fsize = fs::file_size(disk_p, ec);
        if (ec) continue;

        std::vector<char> data;
        bool read_ok = false;
        uint64_t h64 = 0;

        try {
            data.resize(fsize);
        } catch (...) {
            UI::print_error("Memoria insufficiente: " + disk_path);
            result.skip_count++;
            continue;
        }

        // Calcolo hash XXH64 — usa one-shot come fallback se lo state fallisce
        {
            FilePtr in_f(IO::u8fopen(disk_path, "rb"));
            if (in_f) {
                size_t read_res = fread(data.data(), 1, fsize, in_f);
                if (read_res == fsize) {
                    read_ok = true;
                    // Usa sempre one-shot: piu' semplice e robusto
                    h64 = XXH64(data.data(), fsize, 0);
                }
            } else {
                UI::print_error("Accesso negato: " + disk_path);
            }
        }

        if (!read_ok) {
            result.skip_count++;
            continue;
        }

        ::FileEntry fe;
        fe.name = normalize_path(disk_path);
        fe.meta.orig_size = fsize;
        fe.meta.xxhash = h64;
        fe.meta.codec = static_cast<uint8_t>(CodecSelector::select(disk_path, fsize, level));
        // Conversione timestamp portabile
        fe.meta.timestamp = IO::file_time_to_unix(fs::last_write_time(disk_p, ec));
        if (ec) fe.meta.timestamp = 0;

        result.file_count++;

        if (hash_map.count(h64)) {
            fe.meta.is_duplicate = 1;
            fe.meta.duplicate_of_idx = hash_map[h64];
            result.dup_count++;
            UI::print_verbose("Duplicato rilevato: " + fe.name);
        } else {
            hash_map[h64] = static_cast<uint32_t>(final_toc.size());
            fe.meta.is_duplicate = 0;

            // Gestione file grandi: dividere in sotto-chunk se > CHUNK_THRESHOLD
            if (solid_buf.size() + fsize > CHUNK_THRESHOLD) {
                if (!flush_solid_buf(last_codec)) {
                    IO::safe_remove(temp_path);
                    return {false, "Errore compressione chunk."};
                }
                // Se il file singolo e' > CHUNK_THRESHOLD, dividiamolo in sotto-chunk
                size_t offset = 0;
                last_codec = static_cast<Codec>(fe.meta.codec);
                while (offset + CHUNK_THRESHOLD < data.size()) {
                    std::vector<char> sub_chunk(data.begin() + offset,
                                                 data.begin() + offset + CHUNK_THRESHOLD);
                    if (!flush_solid_buf(last_codec)) {
                        IO::safe_remove(temp_path);
                        return {false, "Errore compressione sub-chunk."};
                    }
                    solid_buf = std::move(sub_chunk);
                    if (!flush_solid_buf(last_codec)) {
                        IO::safe_remove(temp_path);
                        return {false, "Errore compressione sub-chunk."};
                    }
                    offset += CHUNK_THRESHOLD;
                }
                // Parte rimanente del file
                if (offset < data.size()) {
                    solid_buf.insert(solid_buf.end(),
                                     data.begin() + offset, data.end());
                }
            } else {
                last_codec = static_cast<Codec>(fe.meta.codec);
                solid_buf.insert(solid_buf.end(), data.begin(), data.end());
            }

            // Rilascia memoria di data subito (duplicata in solid_buf)
            data.clear();
            data.shrink_to_fit();
            result.bytes_in += fsize;
        }
        final_toc.push_back(fe);
    }

    // ── FLUSH FINALE ──────────────────────────────────────────────────────────
    if (!write_pending_async()) {
        IO::safe_remove(temp_path);
        return {false, "Errore chunk finale (async)."};
    }

    if (!solid_buf.empty()) {
        ChunkResult last = compress_worker(std::move(solid_buf), level, last_codec);
        if (!last.error_msg.empty()) UI::print_warning(last.error_msg);
        if (!last.success || !write_chunk_result(last)) {
            IO::safe_remove(temp_path);
            return {false, "Errore compressione/scrittura chunk finale."};
        }
    }

    ::ChunkHeader end_mark = {0, 0, 0, 0};
    if (fwrite(&end_mark, sizeof(end_mark), 1, f) != 1) {
        IO::safe_remove(temp_path);
        return {false, "Errore scrittura end mark."};
    }

    if (!IO::write_toc(f, h, final_toc)) {
        IO::safe_remove(temp_path);
        return {false, "Errore scrittura TOC."};
    }

    // Chiude il file in modo sicuro (RAII wrapper, assegna nullptr per chiudere)
    f = FilePtr(nullptr);

    if (!IO::atomic_rename(temp_path, archive_path)) {
        UI::print_warning("Rename atomica fallita. File temporaneo valido: " + temp_path);
        return {false, "Errore rename atomica. File temporaneo preservato: " + temp_path};
    }

    auto t_end = std::chrono::steady_clock::now();
    result.elapsed_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count());
    try { result.archive_size = fs::file_size(fs::u8path(archive_path)); } catch (...) {}

    result.ok = true;
    return result;
}

// ═══════════════════════════════════════════════════════════════════════════════
// EXTRACT — v2.04
// ═══════════════════════════════════════════════════════════════════════════════

TarcResult extract(const std::string& arch_path, const std::vector<std::string>& patterns,
                   bool test_only, size_t offset, bool flat_mode,
                   const std::string& output_dir) {
    TarcResult result;
    result.ok = true;
    auto t_start = std::chrono::steady_clock::now();

    // Validazione directory di output
    if (!output_dir.empty() && !IO::validate_output_dir(output_dir)) {
        return {false, "Directory di output non valida (contiene '..' o caratteri illegali)."};
    }

    // Crea la directory di output se specificata
    if (!test_only && !output_dir.empty()) {
        try {
            fs::path od = fs::u8path(output_dir);
            if (!fs::exists(od)) fs::create_directories(od);
        } catch (...) {
            return {false, "Impossibile creare la directory di output: " + output_dir};
        }
    }

    FilePtr f(IO::u8fopen(arch_path, "rb"));
    if (!f) return {false, "Archivio non trovato."};

    if (offset > 0) {
        if (!IO::seek64(f, static_cast<int64_t>(offset), SEEK_SET))
            return {false, "Errore seek offset."};
    }

    ::Header h;
    if (fread(&h, sizeof(h), 1, f) != 1) return {false, "Header corrotto o illeggibile."};
    if (!IO::validate_header(h)) return {false, "File non e' un archivio TARC valido."};

    std::vector<::FileEntry> toc;
    h.toc_offset += offset;
    if (!IO::read_toc(f, h, toc)) return {false, "Impossibile leggere TOC."};
    if (!IO::seek64(f, static_cast<int64_t>(offset + sizeof(::Header)), SEEK_SET))
        return {false, "Errore seek dati."};

    // ── PRE-SCAN: identifica originali necessari per i duplicati richiesti ─────
    // Se un duplicato corrisponde ai filtri ma il suo originale no,
    // forziamo l'estrazione dell'originale per poter copiare il duplicato.
    std::set<size_t> force_extract_set;
    auto matches_pattern = [&](const ::FileEntry& fe) -> bool {
        if (patterns.empty()) return true;
        for (const auto& pat : patterns) {
            if (match_pattern(fe.name, pat)) return true;
        }
        return false;
    };

    for (size_t i = 0; i < toc.size(); ++i) {
        if (toc[i].meta.is_duplicate && matches_pattern(toc[i])) {
            uint32_t orig_idx = toc[i].meta.duplicate_of_idx;
            if (orig_idx < static_cast<uint32_t>(toc.size()) && !matches_pattern(toc[orig_idx])) {
                force_extract_set.insert(orig_idx);
            }
        }
    }

    std::vector<char> current_block;
    size_t block_pos = 0;
    std::map<std::string, int> flat_names_counter;
    std::map<uint32_t, std::string> extracted_paths;

    // Buffer temporaneo per originali force-estratti (non scritti su disco,
    // ma necessari per copiare i duplicati)
    std::map<uint32_t, std::vector<char>> original_data_cache;

    std::future<bool> write_future;
    bool write_future_active = false;

    auto wait_write = [&]() -> bool {
        if (!write_future_active) return true;
        bool ok = write_future.get();
        write_future_active = false;
        return ok;
    };

    auto async_write = [&](ExtractTask task) -> bool {
        if (!wait_write()) return false;
        write_future = std::async(std::launch::async, [task]() -> bool {
            if (!IO::write_file_to_disk(task.final_path, task.data.data(),
                                         task.data.size(), task.timestamp)) {
                UI::print_warning("Impossibile scrivere: " + task.final_path);
                return false;
            }
            return true;
        });
        write_future_active = true;
        return true;
    };

    // Accumula nomi dei file corrotti per report finale
    std::vector<std::string> corrupt_files;

    for (size_t i = 0; i < toc.size(); ++i) {
        auto& fe = toc[i];

        // Determina se questo file deve essere estratto
        bool should_extract = matches_pattern(fe) || force_extract_set.count(i) > 0;

        // ── DUPLICATI ────────────────────────────────────────────────────────
        if (fe.meta.is_duplicate) {
            if (should_extract) {
                std::string final_path = build_extract_path(
                    fe.name, flat_mode, output_dir, flat_names_counter, test_only);

                if (!test_only && final_path.empty()) {
                    UI::print_warning("Percorso pericoloso saltato: " + fe.name);
                    result.skip_count++;
                    continue;
                }

                if (test_only) {
                    // Per i duplicati in modalita' test, assumiamo OK
                    // (i dati reali sono nell'originale)
                    UI::print_progress(i + 1, toc.size(), fe.name, 1);
                } else {
                    UI::print_progress(i + 1, toc.size(), fe.name);

                    // Cerca il path dell'originale estratto
                    auto it = extracted_paths.find(fe.meta.duplicate_of_idx);

                    // Oppure cerca nei dati cache degli originali force-estratti
                    auto cache_it = original_data_cache.find(fe.meta.duplicate_of_idx);

                    if (it != extracted_paths.end()) {
                        try {
                            fs::path p = fs::u8path(final_path);
                            if (p.has_parent_path()) {
                                std::error_code ec;
                                fs::create_directories(p.parent_path(), ec);
                            }
                            fs::copy_file(fs::u8path(it->second), p, fs::copy_options::overwrite_existing);
                        } catch (...) {
                            UI::print_warning("Impossibile copiare duplicato: " + fe.name);
                        }
                    } else if (cache_it != original_data_cache.end()) {
                        // Originale non scritto su disco ma disponibile in cache
                        IO::write_file_to_disk(final_path, cache_it->second.data(),
                                                cache_it->second.size(), fe.meta.timestamp);
                    } else {
                        UI::print_warning("Originale non disponibile per duplicato: " + fe.name +
                                          " (idx=" + std::to_string(fe.meta.duplicate_of_idx) + ")");
                    }
                }
                result.bytes_out += fe.meta.orig_size;
                result.dup_count++;
                result.file_count++;
                extracted_paths[static_cast<uint32_t>(i)] = final_path;
            }
            continue;
        }

        // ── NON-DUPLICATO ──────────────────────────────────────────────────────
        std::vector<char> file_data;
        size_t remaining = fe.meta.orig_size;
        size_t src_pos = block_pos;

        // Pre-riserva memoria per evitare riallocazioni
        if (should_extract && fe.meta.orig_size > 0) {
            file_data.reserve(fe.meta.orig_size);
        }

        while (remaining > 0) {
            if (src_pos >= current_block.size()) {
                DecodedChunk chunk = read_next_chunk(f);
                if (!chunk.valid) {
                    wait_write();
                    return {false, "Errore lettura chunk durante estrazione."};
                }
                current_block = std::move(chunk.data);
                src_pos = 0;
            }

            size_t available = current_block.size() - src_pos;
            size_t to_copy = std::min(available, remaining);

            if (should_extract) {
                file_data.insert(file_data.end(),
                    current_block.begin() + src_pos,
                    current_block.begin() + src_pos + to_copy);
            }

            src_pos += to_copy;
            remaining -= to_copy;
        }
        block_pos = src_pos;

        if (!should_extract) continue;

        std::string final_path = build_extract_path(
            fe.name, flat_mode, output_dir, flat_names_counter, test_only);

        if (!test_only && final_path.empty()) {
            UI::print_warning("Percorso pericoloso saltato: " + fe.name);
            result.skip_count++;
            continue;
        }

        // Se questo file e' nell'insieme force_extract ma l'utente non lo ha
        // richiesto direttamente, salvalo in cache senza scriverlo su disco
        bool is_force_extracted = force_extract_set.count(i) > 0 && !matches_pattern(fe);

        if (!test_only && !is_force_extracted) {
            ExtractTask task;
            task.toc_index = i;
            task.final_path = final_path;
            task.data = std::move(file_data);
            task.timestamp = fe.meta.timestamp;
            task.is_duplicate = false;
            task.xxhash = fe.meta.xxhash;
            task.orig_size = fe.meta.orig_size;
            if (!async_write(std::move(task))) {
                return {false, "Errore scrittura file."};
            }
            extracted_paths[static_cast<uint32_t>(i)] = final_path;
        } else if (is_force_extracted) {
            // Salva in cache per i duplicati, non scrivere su disco
            original_data_cache[static_cast<uint32_t>(i)] = std::move(file_data);
        }

        if (test_only) {
            // Verifica hash e mostra su barra
            int test_ok = 1;
            if (fe.meta.xxhash != 0) {
                XXH64_hash_t computed = XXH64(file_data.data(), file_data.size(), 0);
                test_ok = (computed == fe.meta.xxhash) ? 1 : 0;
                if (!test_ok) {
                    corrupt_files.push_back(fe.name);
                }
            }
            UI::print_progress(i + 1, toc.size(), fe.name, test_ok);
        } else {
            UI::print_progress(i + 1, toc.size(), fe.name);
        }

        result.bytes_out += fe.meta.orig_size;
        result.file_count++;
    }

    wait_write();

    // Report file corrotti accumulati
    if (!corrupt_files.empty()) {
        result.ok = false;
        result.message = "Hash non corrispondente per: " + corrupt_files[0];
        if (corrupt_files.size() > 1) {
            result.message += " (e altri " + std::to_string(corrupt_files.size() - 1) + ")";
        }
    }

    auto t_end = std::chrono::steady_clock::now();
    result.elapsed_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count());
    try { result.archive_size = fs::file_size(fs::u8path(arch_path)); } catch (...) {}

    return result;
}

// ═══════════════════════════════════════════════════════════════════════════════
// LIST — Elenca contenuto archivio
// ═══════════════════════════════════════════════════════════════════════════════

TarcResult list(const std::string& arch_path, size_t offset) {
    TarcResult res;
    auto t_start = std::chrono::steady_clock::now();

    FilePtr f(IO::u8fopen(arch_path, "rb"));
    if (!f) return {false, "Errore apertura archivio."};
    if (offset > 0) {
        if (!IO::seek64(f, static_cast<int64_t>(offset), SEEK_SET))
            return {false, "Errore seek."};
    }
    ::Header h;
    if (fread(&h, sizeof(h), 1, f) != 1) return {false, "Errore Header"};
    if (!IO::validate_header(h)) return {false, "File non e' un archivio TARC valido."};

    std::vector<::FileEntry> toc;
    h.toc_offset += offset;
    if (!IO::read_toc(f, h, toc)) return {false, "Errore TOC"};

    for (const auto& fe : toc) {
        UI::print_list_entry(fe.name, fe.meta.orig_size, fe.meta.is_duplicate ? 0 : 1, static_cast<Codec>(fe.meta.codec));
        res.file_count++;
        if (fe.meta.is_duplicate) res.dup_count++;
        res.bytes_in += fe.meta.orig_size;
    }

    auto t_end = std::chrono::steady_clock::now();
    res.elapsed_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count());
    try { res.archive_size = fs::file_size(fs::u8path(arch_path)); } catch (...) {}

    res.ok = true;
    return res;
}

// ═══════════════════════════════════════════════════════════════════════════════
// REMOVE — Streaming, un file alla volta in memoria, con compressione async
// ═══════════════════════════════════════════════════════════════════════════════

TarcResult remove_files(const std::string& arch_path, const std::vector<std::string>& patterns) {
    TarcResult result;
    auto t_start = std::chrono::steady_clock::now();

    // 1. Apri sorgente e leggi header + TOC
    FilePtr f_src(IO::u8fopen(arch_path, "rb"));
    if (!f_src) return {false, "Impossibile aprire l'archivio."};

    ::Header h;
    if (fread(&h, sizeof(h), 1, f_src) != 1) return {false, "Header corrotto."};
    if (!IO::validate_header(h)) return {false, "File non e' un archivio TARC valido."};

    std::vector<::FileEntry> toc;
    if (!IO::read_toc(f_src, h, toc)) return {false, "Impossibile leggere TOC."};

    // 2. Identifica file da rimuovere
    std::set<size_t> remove_set;
    for (size_t i = 0; i < toc.size(); ++i) {
        for (const auto& pat : patterns) {
            if (match_pattern(toc[i].name, pat)) {
                remove_set.insert(i);
                break;
            }
        }
    }

    if (remove_set.empty()) return {false, "Nessun file corrisponde ai pattern specificati."};

    UI::print_verbose("Rimozione di " + std::to_string(remove_set.size()) + " file dall'archivio.");

    // 3. Posizionati all'inizio dei dati
    if (!IO::seek64(f_src, static_cast<int64_t>(sizeof(::Header)), SEEK_SET))
        return {false, "Errore seek dati."};

    // 4. Crea file temporaneo per la riscrittura
    std::string temp_path = IO::make_temp_path(arch_path);
    FilePtr f_dst(IO::u8fopen(temp_path, "wb"));
    if (!f_dst) return {false, "Impossibile creare archivio temporaneo."};

    ::Header new_h{};
    memcpy(new_h.magic, TARC_MAGIC, 4);
    new_h.version = TARC_VERSION;
    if (fwrite(&new_h, sizeof(new_h), 1, f_dst) != 1) {
        IO::safe_remove(temp_path);
        return {false, "Errore scrittura header."};
    }

    // 5. Elaborazione streaming — un file alla volta
    std::vector<char> current_block;
    size_t block_pos = 0;
    std::vector<::FileEntry> new_toc;
    std::map<uint64_t, uint32_t> new_hash_map;
    std::vector<char> solid_buf;
    solid_buf.reserve(CHUNK_THRESHOLD);
    Codec last_codec = Codec::LZMA;
    int level = 6;

    // Compressione async (stesso pattern di compress())
    std::future<ChunkResult> future_chunk;
    bool worker_active = false;

    auto write_chunk_result = [&](ChunkResult& cr) -> bool {
        if (!cr.success) return false;
        if (cr.raw_size > UINT32_MAX || cr.compressed_data.size() > UINT32_MAX) {
            UI::print_error("Chunk troppo grande (> 4 GB) durante rimozione.");
            return false;
        }
        ::ChunkHeader ch = { static_cast<uint32_t>(cr.codec), static_cast<uint32_t>(cr.raw_size),
                              static_cast<uint32_t>(cr.compressed_data.size()), 0 };
        ch.checksum = XXH64(cr.compressed_data.data(), cr.compressed_data.size(), 0);
        if (fwrite(&ch, sizeof(ch), 1, f_dst) != 1) return false;
        if (fwrite(cr.compressed_data.data(), 1, cr.compressed_data.size(), f_dst) != cr.compressed_data.size())
            return false;
        result.bytes_out += cr.compressed_data.size();
        result.codec_bytes[cr.codec] += cr.compressed_data.size();
        result.codec_chunks[cr.codec]++;
        return true;
    };

    auto write_pending_async = [&]() -> bool {
        if (!worker_active) return true;
        ChunkResult cr = future_chunk.get();
        worker_active = false;
        if (!cr.error_msg.empty()) UI::print_warning(cr.error_msg);
        return write_chunk_result(cr);
    };

    auto flush_solid_buf = [&](Codec codec) -> bool {
        if (solid_buf.empty()) return true;
        if (!write_pending_async()) return false;
        future_chunk = std::async(std::launch::async, compress_worker,
                                   std::move(solid_buf), level, codec);
        worker_active = true;
        solid_buf.clear();
        solid_buf.reserve(CHUNK_THRESHOLD);
        return true;
    };

    for (size_t i = 0; i < toc.size(); ++i) {
        auto& fe = toc[i];

        // Per i duplicati, non serve leggere dati dai chunk
        if (fe.meta.is_duplicate) {
            if (remove_set.find(i) != remove_set.end()) {
                UI::print_delete(fe.name);
                continue;
            }
            // Verifica che l'originale non sia rimosso
            uint32_t orig_idx = fe.meta.duplicate_of_idx;
            if (remove_set.find(orig_idx) != remove_set.end()) {
                UI::print_warning("Duplicato orfano (originale rimosso): " + fe.name);
                result.skip_count++;
                continue;
            }
            // Aggiorna il riferimento al nuovo indice TOC
            ::FileEntry new_fe = fe;
            auto it = new_hash_map.find(toc[orig_idx].meta.xxhash);
            if (it != new_hash_map.end()) {
                new_fe.meta.duplicate_of_idx = it->second;
            }
            result.file_count++;
            result.dup_count++;
            new_toc.push_back(new_fe);
            continue;
        }

        // Non-duplicato: leggi i dati dal sorgente (streaming, un file alla volta)
        std::vector<char> file_data;
        size_t remaining = fe.meta.orig_size;

        bool should_keep = remove_set.find(i) == remove_set.end();
        if (should_keep && fe.meta.orig_size > 0) {
            file_data.reserve(fe.meta.orig_size);
        }

        while (remaining > 0) {
            if (block_pos >= current_block.size()) {
                DecodedChunk chunk = read_next_chunk(f_src);
                if (!chunk.valid) {
                    write_pending_async();
                    IO::safe_remove(temp_path);
                    return {false, "Errore lettura chunk durante rimozione."};
                }
                current_block = std::move(chunk.data);
                block_pos = 0;
            }

            size_t available = current_block.size() - block_pos;
            size_t to_copy = std::min(available, remaining);

            if (should_keep) {
                file_data.insert(file_data.end(),
                    current_block.begin() + block_pos,
                    current_block.begin() + block_pos + to_copy);
            }

            block_pos += to_copy;
            remaining -= to_copy;
        }

        if (!should_keep) {
            UI::print_delete(fe.name);
            continue;
        }

        // Mantieni il file: ricalcola hash e scrivi nel nuovo archivio
        ::FileEntry new_fe = fe;
        new_fe.meta.xxhash = XXH64(file_data.data(), file_data.size(), 0);
        new_hash_map[new_fe.meta.xxhash] = static_cast<uint32_t>(new_toc.size());
        new_fe.meta.is_duplicate = 0;

        Codec chosen = static_cast<Codec>(new_fe.meta.codec);

        // Gestione solid buffer con sub-chunk per file grandi
        if (solid_buf.size() + file_data.size() > CHUNK_THRESHOLD && !solid_buf.empty()) {
            if (!flush_solid_buf(last_codec)) {
                IO::safe_remove(temp_path);
                return {false, "Errore ricompressione chunk."};
            }
        }

        if (file_data.size() > CHUNK_THRESHOLD) {
            // File grande: dividi in sotto-chunk
            size_t off = 0;
            last_codec = chosen;
            while (off + CHUNK_THRESHOLD < file_data.size()) {
                std::vector<char> sub(file_data.begin() + off,
                                       file_data.begin() + off + CHUNK_THRESHOLD);
                if (!flush_solid_buf(last_codec)) {
                    IO::safe_remove(temp_path);
                    return {false, "Errore ricompressione sub-chunk."};
                }
                solid_buf = std::move(sub);
                if (!flush_solid_buf(last_codec)) {
                    IO::safe_remove(temp_path);
                    return {false, "Errore ricompressione sub-chunk."};
                }
                off += CHUNK_THRESHOLD;
            }
            if (off < file_data.size()) {
                solid_buf.insert(solid_buf.end(), file_data.begin() + off, file_data.end());
            }
        } else {
            last_codec = chosen;
            solid_buf.insert(solid_buf.end(), file_data.begin(), file_data.end());
        }

        file_data.clear();
        file_data.shrink_to_fit();
        result.bytes_in += fe.meta.orig_size;
        result.file_count++;
        new_toc.push_back(new_fe);
    }

    // Flush finale
    if (!write_pending_async()) {
        IO::safe_remove(temp_path);
        return {false, "Errore ricompressione chunk finale (async)."};
    }

    if (!solid_buf.empty()) {
        ChunkResult cr = compress_worker(std::move(solid_buf), level, last_codec);
        if (!cr.error_msg.empty()) UI::print_warning(cr.error_msg);
        if (!cr.success || !write_chunk_result(cr)) {
            IO::safe_remove(temp_path);
            return {false, "Errore ricompressione chunk finale."};
        }
    }

    // End mark e TOC
    ::ChunkHeader end_mark = {0, 0, 0, 0};
    if (fwrite(&end_mark, sizeof(end_mark), 1, f_dst) != 1) {
        IO::safe_remove(temp_path);
        return {false, "Errore scrittura end mark."};
    }

    if (!IO::write_toc(f_dst, new_h, new_toc)) {
        IO::safe_remove(temp_path);
        return {false, "Errore scrittura TOC."};
    }

    // Chiudi entrambi i file in modo sicuro
    f_dst = FilePtr(nullptr);
    f_src = FilePtr(nullptr);

    if (!IO::atomic_rename(temp_path, arch_path)) {
        UI::print_warning("Rename atomica fallita. File temporaneo: " + temp_path);
        return {false, "Errore rename atomica."};
    }

    auto t_end = std::chrono::steady_clock::now();
    result.elapsed_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count());
    try { result.archive_size = fs::file_size(fs::u8path(arch_path)); } catch (...) {}

    result.ok = true;
    return result;
}

} // namespace Engine
