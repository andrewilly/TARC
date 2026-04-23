#include "engine.h"
#include "io.h"
#include "ui.h"
#include "types.h"
#include <cstring>
#include <map>
#include <filesystem>
#include <fstream>
#include <vector>
#include <iostream>
#include <chrono>
#include <future>
#include <algorithm>
#include <set>

// ─── INCLUDE CODEC ─────────────────────────────────────────────────────────────
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

std::string normalize_path(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}

// ─── INTERVENTO #19: RESOLVE WILDCARDS UNICODE-AWARE ──────────────────────────
// Cross-platform: usa fs::u8path + fs::directory_iterator al posto di
// FindFirstFileA (Windows) che non supporta percorsi Unicode.
void resolve_wildcards(const std::string& pattern, std::vector<std::string>& out) {
    // Se il pattern contiene wildcard, separa directory e filtro
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
                // Filtra il filename contro il pattern
                if (filter.find('*') != std::string::npos || filter.find('?') != std::string::npos) {
                    // Semplice match: confronta con il filtro
                    if (name.find(filter.substr(0, filter.find('*'))) == 0 ||
                        filter == "*" || filter == "*.*") {
                        out.push_back(full);
                    }
                } else {
                    out.push_back(full);
                }
            } else if (entry.is_directory(ec)) {
                // Aggiungi ricorsivamente i file nella sottodirectory
                for (auto& sub : fs::recursive_directory_iterator(entry.path(), ec))
                    if (sub.is_regular_file(ec))
                        out.push_back(normalize_path(sub.path().string()));
            }
        }
    } else {
        // Nessun wildcard: path diretto
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

bool match_pattern(const std::string& full_path, const std::string& pattern) {
    if (pattern.empty()) return true;

    std::string target = full_path;
    if (pattern.find('/') == std::string::npos && pattern.find('\\') == std::string::npos) {
        target = fs::path(full_path).filename().string();
    }

    size_t star_pos = pattern.find('*');

    if (star_pos == std::string::npos) {
        return (target.find(pattern) != std::string::npos);
    }

    std::string prefix = pattern.substr(0, star_pos);
    std::string suffix = pattern.substr(star_pos + 1);

    if (!prefix.empty() && target.find(prefix) != 0) return false;

    if (!suffix.empty()) {
        if (suffix.length() > target.length()) return false;
        if (target.compare(target.length() - suffix.length(), suffix.length(), suffix) != 0) return false;
    }

    return true;
}

bool is_excluded(const std::string& path, const std::vector<std::string>& exclude_patterns) {
    for (const auto& pat : exclude_patterns) {
        if (match_pattern(path, pat)) return true;
    }
    return false;
}

const std::set<std::string>& incompressible_extensions() {
//    static const std::set<std::string> skip = {
//        ".zip", ".7z", ".rar", ".gz", ".bz2", ".xz", ".zst", ".lz4",
//        ".br", ".tar", ".tgz", ".tbz2", ".txz", ".cab", ".arj",
//        ".jpg", ".jpeg", ".png", ".gif", ".webp", ".ico", ".heic",
//        ".heif", ".avif", ".jxl",
//        ".mp3", ".mp4", ".ogg", ".flac", ".aac", ".wma", ".wmv",
//        ".avi", ".mkv", ".mov", ".webm", ".opus", ".m4a", ".m4v",
//        ".pdf", ".docx", ".xlsx", ".pptx", ".odt", ".ods", ".odp",
//        ".epub", ".xps",
//        ".woff", ".woff2", ".ttf", ".otf", ".eot",
//        ".exe", ".dll", ".so", ".dylib", ".nupkg", ".jar", ".apk",
//        ".msi", ".crx",
//        ".ktx", ".ktx2", ".basis", ".dds", ".crn"
//   };
        static const std::set<std::string> skip = {
        ".zip", ".7z", ".rar", ".gz", ".bz2", ".xz", ".zst", ".lz4",
        ".br", ".tar", ".tgz", ".tbz2", ".txz", ".cab", ".arj",
        ".heif", ".avif", ".jxl",
        ".mp3", ".mp4", ".ogg", ".flac", ".aac", ".wma", ".wmv",
        ".avi", ".mkv", ".mov", ".webm", ".opus", ".m4a", ".m4v",
        ".woff", ".woff2", ".ttf", ".otf", ".eot",
        ".msi", ".crx",
        ".ktx", ".ktx2", ".basis", ".dds", ".crn"
    };
    return skip;
}

// ═══════════════════════════════════════════════════════════════════════════════
// COMPRESS WORKER MULTI-CODEC
// ═══════════════════════════════════════════════════════════════════════════════

struct ChunkResult {
    std::vector<char> compressed_data;
    uint32_t raw_size;
    Codec codec;
    bool success;
};

ChunkResult compress_worker(std::vector<char> raw_data, int level, Codec chosen_codec) {
    ChunkResult res;
    res.raw_size = static_cast<uint32_t>(raw_data.size());
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
                res.compressed_data = std::move(raw_data);
                res.codec = Codec::STORE;
            }
            res.success = true;
        } else {
            UI::print_warning("ZSTD failed, falling back to STORE for chunk.");
            res.compressed_data = std::move(raw_data);
            res.codec = Codec::STORE;
            res.success = true;
        }
        return res;
    }

    if (chosen_codec == Codec::LZ4) {
        int max_out = LZ4_compressBound(static_cast<int>(raw_data.size()));
        if (max_out <= 0) {
            res.compressed_data = std::move(raw_data);
            res.codec = Codec::STORE;
            res.success = true;
            return res;
        }
        res.compressed_data.resize(static_cast<size_t>(max_out));
        int result = LZ4_compress_default(
            raw_data.data(), res.compressed_data.data(),
            static_cast<int>(raw_data.size()), max_out);
        if (result > 0) {
            res.compressed_data.resize(static_cast<size_t>(result));
            if (res.compressed_data.size() >= raw_data.size()) {
                res.compressed_data = std::move(raw_data);
                res.codec = Codec::STORE;
            }
            res.success = true;
        } else {
            UI::print_warning("LZ4 failed, falling back to STORE for chunk.");
            res.compressed_data = std::move(raw_data);
            res.codec = Codec::STORE;
            res.success = true;
        }
        return res;
    }

    if (chosen_codec == Codec::BR) {
        size_t max_out = BrotliEncoderMaxCompressedSize(raw_data.size());
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
                res.compressed_data = std::move(raw_data);
                res.codec = Codec::STORE;
            }
            res.success = true;
        } else {
            UI::print_warning("Brotli failed, falling back to STORE for chunk.");
            res.compressed_data = std::move(raw_data);
            res.codec = Codec::STORE;
            res.success = true;
        }
        return res;
    }

    if (chosen_codec == Codec::LZMA) {
        size_t max_out = lzma_stream_buffer_bound(raw_data.size());
        res.compressed_data.resize(max_out);
        size_t out_pos = 0;
        uint32_t preset = (level < 0) ? 9 : static_cast<uint32_t>(level);
        lzma_ret ret = lzma_easy_buffer_encode(
            preset | LZMA_PRESET_EXTREME, LZMA_CHECK_CRC64, NULL,
            reinterpret_cast<const uint8_t*>(raw_data.data()), raw_data.size(),
            reinterpret_cast<uint8_t*>(res.compressed_data.data()), &out_pos, max_out);
        if (ret == LZMA_OK) {
            res.compressed_data.resize(out_pos);
            if (res.compressed_data.size() >= raw_data.size()) {
                res.compressed_data = std::move(raw_data);
                res.codec = Codec::STORE;
            }
            res.success = true;
        } else {
            UI::print_warning("LZMA failed, falling back to STORE for chunk.");
            res.compressed_data = std::move(raw_data);
            res.codec = Codec::STORE;
            res.success = true;
        }
        return res;
    }

    res.compressed_data = std::move(raw_data);
    res.codec = Codec::STORE;
    res.success = true;
    return res;
}

// ═══════════════════════════════════════════════════════════════════════════════
// DECOMPRESS HELPER — Multi-codec
// ═══════════════════════════════════════════════════════════════════════════════

bool decompress_chunk(const std::vector<char>& comp_data, uint32_t codec,
                      std::vector<char>& out_data, uint32_t raw_size) {
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
// READ NEXT CHUNK HELPER — Con verifica checksum
// ═══════════════════════════════════════════════════════════════════════════════

struct DecodedChunk {
    std::vector<char> data;
    bool valid = false;
};

DecodedChunk read_next_chunk(FILE* f) {
    DecodedChunk result;
    ::ChunkHeader ch;

    if (fread(&ch, sizeof(ch), 1, f) != 1 || ch.raw_size == 0) return result;

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
// Intervento #19: Unicode path support (u8fopen, fs::u8path)
// ═══════════════════════════════════════════════════════════════════════════════

namespace Engine {

TarcResult create_sfx(const std::string& archive_path, const std::string& sfx_name) {
    std::string stub_path = "tarc_sfx_stub.exe";
    if (!fs::exists(fs::u8path(stub_path))) return {false, "Stub SFX non trovato."};

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

    sfx_out << stub_in.rdbuf();
    sfx_out << data_in.rdbuf();

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

    // ── APPEND: leggi e VALIDA TOC esistente ──────────────────────────────────
    if (append && fs::exists(fs::u8path(archive_path))) {
        // INTERVENTO #19: u8fopen per percorsi Unicode
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
    std::string actual_write_path = archive_path;
    std::string temp_path;
    bool using_temp = !append;

    if (append) {
        temp_path = IO::make_temp_path(archive_path);
        try {
            fs::copy_file(fs::u8path(archive_path), fs::u8path(temp_path), fs::copy_options::overwrite_existing);
        } catch (...) {
            return {false, "Impossibile creare file temporaneo per append atomico."};
        }
        actual_write_path = temp_path;
        using_temp = true;
    } else {
        temp_path = IO::make_temp_path(archive_path);
        actual_write_path = temp_path;
        using_temp = true;
    }

    // INTERVENTO #19: u8fopen per percorsi Unicode
    FilePtr f(IO::u8fopen(actual_write_path, append ? "rb+" : "wb"));
    if (!f) {
        if (using_temp && !temp_path.empty()) IO::safe_remove(temp_path);
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
        ::ChunkHeader ch = { static_cast<uint32_t>(res.codec), res.raw_size,
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

        XXH64_state_t* const state = XXH64_createState();
        if (state) XXH64_reset(state, 0);

        // INTERVENTO #19: lettura file cross-platform Unicode-aware
        // Usa IO::u8fopen al posto di CreateFileA/fopen con #ifdef
        {
            FilePtr in_f(IO::u8fopen(disk_path, "rb"));
            if (in_f) {
                size_t read_res = fread(data.data(), 1, fsize, in_f);
                if (read_res == fsize) {
                    read_ok = true;
                    if (state) XXH64_update(state, data.data(), fsize);
                }
            } else {
                UI::print_error("Accesso negato: " + disk_path);
            }
        }

        if (state) {
            h64 = XXH64_digest(state);
            XXH64_freeState(state);
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
        fe.meta.timestamp = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
            fs::last_write_time(disk_p, ec).time_since_epoch()).count());

        result.file_count++;

        if (hash_map.count(h64)) {
            fe.meta.is_duplicate = 1;
            fe.meta.duplicate_of_idx = hash_map[h64];
            result.dup_count++;
            UI::print_verbose("Duplicato rilevato: " + fe.name);
        } else {
            hash_map[h64] = static_cast<uint32_t>(final_toc.size());
            fe.meta.is_duplicate = 0;

            if (solid_buf.size() + fsize > CHUNK_THRESHOLD && !solid_buf.empty()) {
                if (!flush_solid_buf(last_codec)) {
                    IO::safe_remove(temp_path);
                    return {false, "Errore compressione chunk."};
                }
            }

            last_codec = static_cast<Codec>(fe.meta.codec);
            solid_buf.insert(solid_buf.end(), data.begin(), data.end());
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

    f.~FilePtr();

    if (using_temp && !temp_path.empty()) {
        if (!IO::atomic_rename(temp_path, archive_path)) {
            UI::print_warning("Rename atomica fallita. File temporaneo valido: " + temp_path);
            return {false, "Errore rename atomica. File temporaneo preservato: " + temp_path};
        }
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

    // INTERVENTO #19: u8fopen per percorsi Unicode
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

    std::vector<char> current_block;
    size_t block_pos = 0;
    std::map<std::string, int> flat_names_counter;
    std::map<uint32_t, std::string> extracted_paths;

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

    for (size_t i = 0; i < toc.size(); ++i) {
        auto& fe = toc[i];

        bool should_extract = patterns.empty();
        if (!should_extract) {
            for (const auto& pat : patterns) {
                if (match_pattern(fe.name, pat)) { should_extract = true; break; }
            }
        }

        // ── DUPLICATI ────────────────────────────────────────────────────────
        if (fe.meta.is_duplicate) {
            if (should_extract) {
                std::string final_path = fe.name;
                if (flat_mode) {
                    fs::path p(fe.name);
                    std::string filename = p.filename().string();
                    if (flat_names_counter.count(filename)) {
                        flat_names_counter[filename]++;
                        size_t dot_pos = filename.find_last_of('.');
                        if (dot_pos != std::string::npos) {
                            filename = filename.substr(0, dot_pos) + "_" + std::to_string(flat_names_counter[filename]) + filename.substr(dot_pos);
                        } else {
                            filename += "_" + std::to_string(flat_names_counter[filename]);
                        }
                    } else {
                        flat_names_counter[filename] = 0;
                    }
                    final_path = filename;
                }

                if (!test_only && !output_dir.empty()) {
                    final_path = output_dir + "/" + final_path;
                }

                if (!test_only) {
                    std::string safe_path = IO::sanitize_path(final_path);
                    if (safe_path.empty()) {
                        UI::print_warning("Percorso pericoloso saltato: " + final_path);
                        result.skip_count++;
                        continue;
                    }
                    final_path = safe_path;
                }

                UI::print_progress(i + 1, toc.size(), fe.name);

                if (!test_only) {
                    auto it = extracted_paths.find(fe.meta.duplicate_of_idx);
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
                    } else {
                        UI::print_warning("Originale non estratto per duplicato: " + fe.name);
                    }
                } else {
                    UI::print_extract(fe.name, fe.meta.orig_size, true, true);
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

        UI::print_progress(i + 1, toc.size(), fe.name);

        std::string final_path = fe.name;
        if (flat_mode) {
            fs::path p(fe.name);
            std::string filename = p.filename().string();
            if (flat_names_counter.count(filename)) {
                flat_names_counter[filename]++;
                size_t dot_pos = filename.find_last_of('.');
                if (dot_pos != std::string::npos) {
                    filename = filename.substr(0, dot_pos) + "_" + std::to_string(flat_names_counter[filename]) + filename.substr(dot_pos);
                } else {
                    filename += "_" + std::to_string(flat_names_counter[filename]);
                }
            } else {
                flat_names_counter[filename] = 0;
            }
            final_path = filename;
        }

        if (!test_only && !output_dir.empty()) {
            final_path = output_dir + "/" + final_path;
        }

        if (!test_only) {
            std::string safe_path = IO::sanitize_path(final_path);
            if (safe_path.empty()) {
                UI::print_warning("Percorso pericoloso saltato: " + final_path);
                result.skip_count++;
                continue;
            }
            final_path = safe_path;

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
        }

        if (test_only && fe.meta.xxhash != 0) {
            XXH64_hash_t computed = XXH64(file_data.data(), file_data.size(), 0);
            bool hash_ok = (computed == fe.meta.xxhash);
            UI::print_extract(fe.name, fe.meta.orig_size, true, hash_ok);
            if (!hash_ok) {
                result.ok = false;
                result.message = "Hash non corrispondente per: " + fe.name;
            }
        }

        result.bytes_out += fe.meta.orig_size;
        result.file_count++;
        extracted_paths[static_cast<uint32_t>(i)] = final_path;
    }

    wait_write();

    auto t_end = std::chrono::steady_clock::now();
    result.elapsed_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count());
    try { result.archive_size = fs::file_size(fs::u8path(arch_path)); } catch (...) {}

    return result;
}

TarcResult list(const std::string& arch_path, size_t offset) {
    TarcResult res;
    auto t_start = std::chrono::steady_clock::now();

    // INTERVENTO #19: u8fopen
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
// REMOVE — v2.04 (Unicode-aware)
// ═══════════════════════════════════════════════════════════════════════════════

TarcResult remove_files(const std::string& arch_path, const std::vector<std::string>& patterns) {
    TarcResult result;
    auto t_start = std::chrono::steady_clock::now();

    FilePtr f_src(IO::u8fopen(arch_path, "rb"));
    if (!f_src) return {false, "Impossibile aprire l'archivio."};

    ::Header h;
    if (fread(&h, sizeof(h), 1, f_src) != 1) return {false, "Header corrotto."};
    if (!IO::validate_header(h)) return {false, "File non e' un archivio TARC valido."};

    std::vector<::FileEntry> toc;
    if (!IO::read_toc(f_src, h, toc)) return {false, "Impossibile leggere TOC."};

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

    if (!IO::seek64(f_src, static_cast<int64_t>(sizeof(::Header)), SEEK_SET))
        return {false, "Errore seek dati."};

    std::map<size_t, std::vector<char>> file_data_map;
    std::vector<char> current_block;
    size_t block_pos = 0;

    for (size_t i = 0; i < toc.size(); ++i) {
        auto& fe = toc[i];
        if (fe.meta.is_duplicate) continue;

        std::vector<char> file_data;
        size_t remaining = fe.meta.orig_size;

        while (remaining > 0) {
            if (block_pos >= current_block.size()) {
                DecodedChunk chunk = read_next_chunk(f_src);
                if (!chunk.valid) return {false, "Errore lettura chunk durante rimozione."};
                current_block = std::move(chunk.data);
                block_pos = 0;
            }

            size_t available = current_block.size() - block_pos;
            size_t to_copy = std::min(available, remaining);

            file_data.insert(file_data.end(),
                current_block.begin() + block_pos,
                current_block.begin() + block_pos + to_copy);

            block_pos += to_copy;
            remaining -= to_copy;
        }

        if (remove_set.find(i) == remove_set.end()) {
            file_data_map[i] = std::move(file_data);
        }
    }

    // Riscrivi l'archivio
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

    std::vector<::FileEntry> new_toc;
    std::map<uint64_t, uint32_t> new_hash_map;
    std::vector<char> solid_buf;
    solid_buf.reserve(CHUNK_THRESHOLD);
    Codec last_codec = Codec::LZMA;
    int level = 6;

    for (size_t i = 0; i < toc.size(); ++i) {
        if (remove_set.find(i) != remove_set.end()) {
            UI::print_delete(toc[i].name);
            continue;
        }

        ::FileEntry fe = toc[i];

        if (fe.meta.is_duplicate) {
            uint32_t orig_idx = fe.meta.duplicate_of_idx;
            if (remove_set.find(orig_idx) != remove_set.end()) {
                UI::print_warning("Duplicato orfano (originale rimosso): " + fe.name);
                result.skip_count++;
                continue;
            }
            auto it = new_hash_map.find(toc[orig_idx].meta.xxhash);
            if (it != new_hash_map.end()) {
                fe.meta.duplicate_of_idx = it->second;
                fe.meta.is_duplicate = 1;
            }
        } else {
            auto data_it = file_data_map.find(i);
            if (data_it == file_data_map.end()) {
                UI::print_warning("Dati mancanti per: " + fe.name);
                result.skip_count++;
                continue;
            }

            fe.meta.xxhash = XXH64(data_it->second.data(), data_it->second.size(), 0);
            new_hash_map[fe.meta.xxhash] = static_cast<uint32_t>(new_toc.size());
            fe.meta.is_duplicate = 0;

            Codec chosen = static_cast<Codec>(fe.meta.codec);

            if (solid_buf.size() + data_it->second.size() > CHUNK_THRESHOLD && !solid_buf.empty()) {
                ChunkResult cr = compress_worker(std::move(solid_buf), level, last_codec);
                if (!cr.success) { IO::safe_remove(temp_path); return {false, "Errore ricompressione chunk."}; }
                ::ChunkHeader ch = { static_cast<uint32_t>(cr.codec), cr.raw_size,
                                      static_cast<uint32_t>(cr.compressed_data.size()), 0 };
                ch.checksum = XXH64(cr.compressed_data.data(), cr.compressed_data.size(), 0);
                fwrite(&ch, sizeof(ch), 1, f_dst);
                fwrite(cr.compressed_data.data(), 1, cr.compressed_data.size(), f_dst);
                result.bytes_out += cr.compressed_data.size();
                result.codec_bytes[cr.codec] += cr.compressed_data.size();
                result.codec_chunks[cr.codec]++;
            }

            last_codec = chosen;
            solid_buf.insert(solid_buf.end(), data_it->second.begin(), data_it->second.end());
            result.bytes_in += data_it->second.size();
        }

        result.file_count++;
        if (fe.meta.is_duplicate) result.dup_count++;
        new_toc.push_back(fe);
    }

    if (!solid_buf.empty()) {
        ChunkResult cr = compress_worker(std::move(solid_buf), level, last_codec);
        if (!cr.success) { IO::safe_remove(temp_path); return {false, "Errore ricompressione chunk finale."}; }
        ::ChunkHeader ch = { static_cast<uint32_t>(cr.codec), cr.raw_size,
                              static_cast<uint32_t>(cr.compressed_data.size()), 0 };
        ch.checksum = XXH64(cr.compressed_data.data(), cr.compressed_data.size(), 0);
        fwrite(&ch, sizeof(ch), 1, f_dst);
        fwrite(cr.compressed_data.data(), 1, cr.compressed_data.size(), f_dst);
        result.bytes_out += cr.compressed_data.size();
        result.codec_bytes[cr.codec] += cr.compressed_data.size();
        result.codec_chunks[cr.codec]++;
    }

    ::ChunkHeader end_mark = {0, 0, 0, 0};
    fwrite(&end_mark, sizeof(end_mark), 1, f_dst);

    if (!IO::write_toc(f_dst, new_h, new_toc)) {
        IO::safe_remove(temp_path);
        return {false, "Errore scrittura TOC."};
    }

    f_dst.~FilePtr();
    f_src.~FilePtr();

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
