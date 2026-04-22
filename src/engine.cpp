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

#ifdef _WIN32
    #include <windows.h>
#endif

// ─── INCLUDE CODEC ─────────────────────────────────────────────────────────────
// Tutti i codec sono ora effettivamente utilizzati, non solo linkati.
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
// Intervento #7: tutte le funzioni helper private sono qui dentro
// ═══════════════════════════════════════════════════════════════════════════════
namespace {

std::string normalize_path(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}

void resolve_wildcards(const std::string& pattern, std::vector<std::string>& out) {
#ifdef _WIN32
    WIN32_FIND_DATAA findData;
    HANDLE hFind = FindFirstFileA(pattern.c_str(), &findData);
    if (hFind == INVALID_HANDLE_VALUE) return;
    std::string directory = "";
    size_t last_slash = pattern.find_last_of("\\/");
    if (last_slash != std::string::npos) directory = pattern.substr(0, last_slash + 1);
    do {
        std::string foundName(findData.cFileName);
        if (foundName != "." && foundName != "..") {
            std::string fullPath = directory + foundName;
            if (fs::exists(fullPath)) {
                if (fs::is_regular_file(fullPath)) out.push_back(normalize_path(fullPath));
                else if (fs::is_directory(fullPath)) {
                    for (auto& p : fs::recursive_directory_iterator(fullPath))
                        if (p.is_regular_file()) out.push_back(normalize_path(p.path().string()));
                }
            }
        }
    } while (FindNextFileA(hFind, &findData));
    FindClose(hFind);
#else
    if (fs::exists(pattern)) {
        if (fs::is_directory(pattern)) {
            for (auto& p : fs::recursive_directory_iterator(pattern))
                if (p.is_regular_file()) out.push_back(normalize_path(p.path().string()));
        } else out.push_back(normalize_path(pattern));
    }
#endif
}

bool match_pattern(const std::string& full_path, const std::string& pattern) {
    if (pattern.empty()) return true;

    // Estrae il nome del file dal percorso completo se il pattern non contiene slash.
    std::string target = full_path;
    if (pattern.find('/') == std::string::npos && pattern.find('\\') == std::string::npos) {
        target = fs::path(full_path).filename().string();
    }

    // Gestione Wildcard *
    size_t star_pos = pattern.find('*');

    if (star_pos == std::string::npos) {
        // Nessun asterisco: corrispondenza parziale (contiene la stringa)
        return (target.find(pattern) != std::string::npos);
    }

    // C'e' un asterisco: formato Prefisso*Suffix
    std::string prefix = pattern.substr(0, star_pos);
    std::string suffix = pattern.substr(star_pos + 1);

    if (!prefix.empty() && target.find(prefix) != 0) return false;

    if (!suffix.empty()) {
        if (suffix.length() > target.length()) return false;
        if (target.compare(target.length() - suffix.length(), suffix.length(), suffix) != 0) return false;
    }

    return true;
}

// ─── LISTA FORMATI INCOMPRESSIBILI ESTESA ─────────────────────────────────────
// Intervento #8: lista significativamente ampliata per evitare compressione
// inutile su file gia' compressi, con risparmio di tempo e dimensione.
const std::set<std::string>& incompressible_extensions() {
    static const std::set<std::string> skip = {
        // Archivi compressi
        ".zip", ".7z", ".rar", ".gz", ".bz2", ".xz", ".zst", ".lz4",
        ".br", ".tar", ".tgz", ".tbz2", ".txz", ".cab", ".arj",
        // Immagini compresse
        ".jpg", ".jpeg", ".png", ".gif", ".webp", ".ico", ".heic",
        ".heif", ".avif", ".jxl",
        // Audio/Video compressi
        ".mp3", ".mp4", ".ogg", ".flac", ".aac", ".wma", ".wmv",
        ".avi", ".mkv", ".mov", ".webm", ".opus", ".m4a", ".m4v",
        // Documenti compressi
        ".pdf", ".docx", ".xlsx", ".pptx", ".odt", ".ods", ".odp",
        ".epub", ".xps",
        // Font
        ".woff", ".woff2", ".ttf", ".otf", ".eot",
        // Eseguibili e pacchetti compressi
        ".exe", ".dll", ".so", ".dylib", ".nupkg", ".jar", ".apk",
        ".msi", ".crx",
        // Video game assets
        ".ktx", ".ktx2", ".basis", ".dds", ".crn"
    };
    return skip;
}

// ═══════════════════════════════════════════════════════════════════════════════
// COMPRESS WORKER MULTI-CODEC — Intervento #1
// Ogni codec e' ora effettivamente implementato, non solo dichiarato.
// Se un codec fallisce, fallback automatico a STORE.
// Se la compressione produce dati piu' grandi dell'originale, fallback a STORE.
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

    // Dati vuoti: successo immediato
    if (raw_data.empty()) {
        res.success = true;
        return res;
    }

    // ── STORE (nessuna compressione) ──────────────────────────────────────────
    if (chosen_codec == Codec::STORE) {
        res.compressed_data = std::move(raw_data);
        res.success = true;
        return res;
    }

    // ── ZSTD ──────────────────────────────────────────────────────────────────
    if (chosen_codec == Codec::ZSTD) {
        size_t max_out = ZSTD_compressBound(raw_data.size());
        res.compressed_data.resize(max_out);
        int zstd_level = std::clamp(level, 1, 19);
        size_t result = ZSTD_compress(
            res.compressed_data.data(), max_out,
            raw_data.data(), raw_data.size(), zstd_level);

        if (!ZSTD_isError(result)) {
            res.compressed_data.resize(result);
            // Se compresso > originale, fallback a STORE
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

    // ── LZ4 ───────────────────────────────────────────────────────────────────
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
            // Se compresso > originale, fallback a STORE
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

    // ── BROTLI ────────────────────────────────────────────────────────────────
    if (chosen_codec == Codec::BR) {
        size_t max_out = BrotliEncoderMaxCompressedSize(raw_data.size());
        if (max_out == 0) {
            // Input troppo grande per Brotli
            res.compressed_data = std::move(raw_data);
            res.codec = Codec::STORE;
            res.success = true;
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
            // Se compresso > originale, fallback a STORE
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

    // ── LZMA ──────────────────────────────────────────────────────────────────
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
            // Se compresso > originale, fallback a STORE
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

    // Codec sconosciuto: STORE
    res.compressed_data = std::move(raw_data);
    res.codec = Codec::STORE;
    res.success = true;
    return res;
}

// ═══════════════════════════════════════════════════════════════════════════════
// DECOMPRESS HELPER — Supporto multi-codec in estrazione
// Intervento #1: decompressione effettiva per tutti i codec
// ═══════════════════════════════════════════════════════════════════════════════

bool decompress_chunk(const std::vector<char>& comp_data, uint32_t codec,
                      std::vector<char>& out_data, uint32_t raw_size) {
    out_data.resize(raw_size);

    // ── STORE ─────────────────────────────────────────────────────────────────
    if (codec == static_cast<uint32_t>(Codec::STORE)) {
        if (raw_size > comp_data.size()) return false;
        memcpy(out_data.data(), comp_data.data(), raw_size);
        return true;
    }

    // ── ZSTD ──────────────────────────────────────────────────────────────────
    if (codec == static_cast<uint32_t>(Codec::ZSTD)) {
        size_t result = ZSTD_decompress(
            out_data.data(), raw_size,
            comp_data.data(), comp_data.size());
        if (ZSTD_isError(result)) return false;
        return true;
    }

    // ── LZ4 ───────────────────────────────────────────────────────────────────
    if (codec == static_cast<uint32_t>(Codec::LZ4)) {
        int result = LZ4_decompress_safe(
            comp_data.data(), out_data.data(),
            static_cast<int>(comp_data.size()),
            static_cast<int>(raw_size));
        return result > 0;
    }

    // ── BROTLI ────────────────────────────────────────────────────────────────
    if (codec == static_cast<uint32_t>(Codec::BR)) {
        size_t decoded_size = raw_size;
        BrotliDecoderResult result = BrotliDecoderDecompress(
            comp_data.size(), reinterpret_cast<const uint8_t*>(comp_data.data()),
            &decoded_size, reinterpret_cast<uint8_t*>(out_data.data()));
        return result == BROTLI_DECODER_RESULT_SUCCESS && decoded_size == raw_size;
    }

    // ── LZMA ──────────────────────────────────────────────────────────────────
    if (codec == static_cast<uint32_t>(Codec::LZMA)) {
        size_t src_p = 0, dst_p = 0;
        uint64_t limit = UINT64_MAX;
        lzma_ret ret = lzma_stream_buffer_decode(
            &limit, 0, NULL,
            reinterpret_cast<const uint8_t*>(comp_data.data()), &src_p, comp_data.size(),
            reinterpret_cast<uint8_t*>(out_data.data()), &dst_p, raw_size);
        return (ret == LZMA_OK || ret == LZMA_STREAM_END);
    }

    // Codec sconosciuto
    return false;
}

// ═══════════════════════════════════════════════════════════════════════════════
// READ NEXT CHUNK HELPER — Intervento #2 + #3
// Elimina la duplicazione di codice in extract().
// Verifica checksum XXH64 se presente (retrocompatibile: checksum=0 → skip).
// ═══════════════════════════════════════════════════════════════════════════════

struct DecodedChunk {
    std::vector<char> data;
    bool valid = false;
};

DecodedChunk read_next_chunk(FILE* f) {
    DecodedChunk result;
    ::ChunkHeader ch;

    if (fread(&ch, sizeof(ch), 1, f) != 1 || ch.raw_size == 0) {
        return result;  // End mark o errore
    }

    std::vector<char> comp(ch.comp_size);
    if (fread(comp.data(), 1, ch.comp_size, f) != ch.comp_size) {
        return result;
    }

    // Intervento #3: Verifica checksum XXH64 se presente
    // Retrocompatibile: vecchi archivi hanno checksum=0, il check viene saltato
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

} // namespace anonimo

// ═══════════════════════════════════════════════════════════════════════════════
// CODEC SELECTOR — Intervento #8
// Selezione intelligente basata su livello, dimensione ed estensione
// ═══════════════════════════════════════════════════════════════════════════════

namespace CodecSelector {
    bool is_compressibile(const std::string& ext) {
        std::string e = ext;
        std::transform(e.begin(), e.end(), e.begin(), ::tolower);
        return incompressible_extensions().find(e) == incompressible_extensions().end();
    }

    // Intervento #1: selezione codec basata sul livello di compressione
    // Level 1-2: LZ4 (velocita' massima)
    // Level 3-5: ZSTD per piccoli, LZMA per grandi (bilanciato)
    // Level 6-9: LZMA sempre (massimo rapporto di compressione)
    Codec select(const std::string& path, size_t size, int level) {
        if (!is_compressibile(fs::path(path).extension().string())) return Codec::STORE;

        if (level <= 2) return Codec::LZ4;
        if (level <= 5) return (size < 512 * 1024) ? Codec::ZSTD : Codec::LZMA;
        return Codec::LZMA;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// ENGINE — Implementazione principale
// ═══════════════════════════════════════════════════════════════════════════════

namespace Engine {

TarcResult create_sfx(const std::string& archive_path, const std::string& sfx_name) {
    std::string stub_path = "tarc_sfx_stub.exe";
    if (!fs::exists(stub_path)) return {false, "Stub SFX (tarc_sfx_stub.exe) non trovato nella cartella."};

    std::ifstream stub_in(stub_path, std::ios::binary);
    std::ifstream data_in(archive_path, std::ios::binary);
    std::ofstream sfx_out(sfx_name, std::ios::binary);

    if (!stub_in || !data_in || !sfx_out) return {false, "Errore fatale durante la fusione SFX."};

    sfx_out << stub_in.rdbuf();
    sfx_out << data_in.rdbuf();

    return {true, "Archivio autoestraente generato."};
}

// ═══════════════════════════════════════════════════════════════════════════════
// COMPRESS — Interventi #1, #3, #4, #6, #10
// - Multi-codec effettivo (#1)
// - Checksum XXH64 nei ChunkHeader (#3)
// - Gestione file > 256MB (#4)
// - RAII FilePtr (#6)
// - Check errori fwrite (#10)
// ═══════════════════════════════════════════════════════════════════════════════

TarcResult compress(const std::string& archive_path, const std::vector<std::string>& inputs, bool append, int level) {
    TarcResult result;
    std::vector<std::string> expanded_files;
    for (const auto& in : inputs) resolve_wildcards(in, expanded_files);
    if (expanded_files.empty()) return {false, "Nessun file trovato."};

    std::vector<::FileEntry> final_toc;
    std::map<uint64_t, uint32_t> hash_map;
    ::Header h{};

    // ── APPEND: leggi TOC esistente ───────────────────────────────────────────
    if (append && fs::exists(archive_path)) {
        FilePtr f_old(fopen(archive_path.c_str(), "rb"));
        if (f_old) {
            if (fread(&h, sizeof(h), 1, f_old) != 1) return {false, "Header archivio corrotto."};
            IO::read_toc(f_old, h, final_toc);
            for (size_t k = 0; k < final_toc.size(); ++k) {
                if (!final_toc[k].meta.is_duplicate) hash_map[final_toc[k].meta.xxhash] = static_cast<uint32_t>(k);
            }
        }
    } else {
        memcpy(h.magic, TARC_MAGIC, 4);
        h.version = TARC_VERSION;
    }

    // Intervento #6: RAII FilePtr — fclose automatico
    FilePtr f(fopen(archive_path.c_str(), append ? "rb+" : "wb"));
    if (!f) return {false, "ERRORE CRITICO: Impossibile scrivere l'archivio."};

    // Intervento #5: seek64 per archivi > 2GB
    if (append) {
        if (!IO::seek64(f, static_cast<int64_t>(h.toc_offset), SEEK_SET))
            return {false, "Errore seek nell'archivio."};
    } else {
        if (fwrite(&h, sizeof(h), 1, f) != 1) return {false, "Errore scrittura header."};
    }

    // ── SOLID BUFFER E GESTIONE CHUNK ─────────────────────────────────────────
    std::vector<char> solid_buf;
    solid_buf.reserve(256 * 1024 * 1024);

    const size_t CHUNK_THRESHOLD = 256 * 1024 * 1024;
    std::future<ChunkResult> future_chunk;
    bool worker_active = false;
    Codec last_codec = Codec::LZMA;  // Traccia l'ultimo codec usato

    // Intervento #3: Scrittura chunk con checksum XXH64
    auto write_chunk_result = [&](ChunkResult& res) -> bool {
        if (!res.success) return false;
        ::ChunkHeader ch = { static_cast<uint32_t>(res.codec), res.raw_size,
                              static_cast<uint32_t>(res.compressed_data.size()), 0 };
        // Checksum XXH64 del dato compresso
        ch.checksum = XXH64(res.compressed_data.data(), res.compressed_data.size(), 0);
        // Intervento #10: check fwrite
        if (fwrite(&ch, sizeof(ch), 1, f) != 1) return false;
        if (fwrite(res.compressed_data.data(), 1, res.compressed_data.size(), f) != res.compressed_data.size())
            return false;
        result.bytes_out += res.compressed_data.size();
        return true;
    };

    // Attende e scrive il chunk async pendente
    auto write_pending_async = [&]() -> bool {
        if (!worker_active) return true;
        ChunkResult res = future_chunk.get();
        worker_active = false;
        return write_chunk_result(res);
    };

    // Svuota il solid buffer avviando compressione async
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

        if (!fs::exists(disk_path)) continue;
        uintmax_t fsize = fs::file_size(disk_path);

        std::vector<char> data;
        bool read_ok = false;
        uint64_t h64 = 0;

        try {
            data.resize(fsize);
        } catch (...) {
            UI::print_error("Memoria insufficiente: " + disk_path);
            continue;
        }

        XXH64_state_t* const state = XXH64_createState();
        if (state) XXH64_reset(state, 0);

#ifdef _WIN32
        HANDLE hFile = CreateFileA(disk_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            DWORD bytesReadTotal = 0;
            DWORD bytesRead = 0;
            const DWORD BUF_STEP = 1024 * 1024;
            char* ptr = data.data();

            while (bytesReadTotal < static_cast<DWORD>(fsize)) {
                DWORD toRead = ((static_cast<DWORD>(fsize) - bytesReadTotal > BUF_STEP) ? BUF_STEP : (static_cast<DWORD>(fsize) - bytesReadTotal));
                if (ReadFile(hFile, ptr + bytesReadTotal, toRead, &bytesRead, NULL)) {
                    if (state) XXH64_update(state, ptr + bytesReadTotal, bytesRead);
                    bytesReadTotal += bytesRead;
                } else {
                    UI::print_error("Errore lettura: " + disk_path + " (WinErr: " + std::to_string(GetLastError()) + ")");
                    break;
                }
            }
            if (bytesReadTotal == static_cast<DWORD>(fsize)) read_ok = true;
            CloseHandle(hFile);
        } else UI::print_error("Accesso negato: " + disk_path);
#else
        FILE* in_f = fopen(disk_path.c_str(), "rb");
        if (in_f) {
            size_t read_res = fread(data.data(), 1, fsize, in_f);
            if (read_res == fsize) {
                read_ok = true;
                if (state) XXH64_update(state, data.data(), fsize);
            }
            fclose(in_f);
        }
#endif

        if (state) {
            h64 = XXH64_digest(state);
            XXH64_freeState(state);
        }

        if (!read_ok) continue;

        ::FileEntry fe;
        fe.name = normalize_path(disk_path);
        fe.meta.orig_size = fsize;
        fe.meta.xxhash = h64;
        fe.meta.codec = static_cast<uint8_t>(CodecSelector::select(disk_path, fsize, level));
        fe.meta.timestamp = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(fs::last_write_time(disk_path).time_since_epoch()).count());

        if (hash_map.count(h64)) {
            fe.meta.is_duplicate = 1;
            fe.meta.duplicate_of_idx = hash_map[h64];
        } else {
            hash_map[h64] = static_cast<uint32_t>(final_toc.size());
            fe.meta.is_duplicate = 0;

            // Intervento #4: Gestione file grandi (> 256MB)
            // Se il file supera la soglia, prima svuota il buffer corrente
            // Il file grande diventera' il suo chunk dedicato
            if (solid_buf.size() + fsize > CHUNK_THRESHOLD && !solid_buf.empty()) {
                if (!flush_solid_buf(last_codec)) return {false, "Errore compressione chunk."};
            }

            last_codec = static_cast<Codec>(fe.meta.codec);
            solid_buf.insert(solid_buf.end(), data.begin(), data.end());
            result.bytes_in += fsize;
        }
        final_toc.push_back(fe);
    }

    // ── FLUSH FINALE ──────────────────────────────────────────────────────────
    // Attende l'ultimo chunk async
    if (!write_pending_async()) return {false, "Errore chunk finale (async)."};

    // Compressione dell'ultimo solid_buf residuo
    if (!solid_buf.empty()) {
        ChunkResult last = compress_worker(std::move(solid_buf), level, last_codec);
        if (!last.success) return {false, "Errore compressione chunk finale."};
        if (!write_chunk_result(last)) return {false, "Errore scrittura chunk finale."};
    }

    // End mark
    ::ChunkHeader end_mark = {0, 0, 0, 0};
    if (fwrite(&end_mark, sizeof(end_mark), 1, f) != 1) return {false, "Errore scrittura end mark."};

    // Scrittura TOC
    if (!IO::write_toc(f, h, final_toc)) return {false, "Errore scrittura TOC."};

    result.ok = true;
    return result;
}

// ═══════════════════════════════════════════════════════════════════════════════
// EXTRACT — Interventi #1, #2, #3, #4, #5, #6
// - Multi-codec decompressione (#1)
// - Refactor con read_next_chunk helper (#2)
// - Verifica checksum (#3)
// - Gestione file spanning su piu' chunk (#4)
// - seek64 per archivi grandi (#5)
// - RAII FilePtr (#6)
// - Estrazione duplicati migliorata
// ═══════════════════════════════════════════════════════════════════════════════

TarcResult extract(const std::string& arch_path, const std::vector<std::string>& patterns, bool test_only, size_t offset, bool flat_mode) {
    TarcResult result;
    result.ok = true;

    FilePtr f(fopen(arch_path.c_str(), "rb"));
    if (!f) return {false, "Archivio non trovato."};

    if (offset > 0) {
        if (!IO::seek64(f, static_cast<int64_t>(offset), SEEK_SET))
            return {false, "Errore seek offset."};
    }

    ::Header h;
    if (fread(&h, sizeof(h), 1, f) != 1) return {false, "Header corrotto o illeggibile."};

    std::vector<::FileEntry> toc;
    h.toc_offset += offset;
    if (!IO::read_toc(f, h, toc)) return {false, "Impossibile leggere TOC."};

    if (!IO::seek64(f, static_cast<int64_t>(offset + sizeof(::Header)), SEEK_SET))
        return {false, "Errore seek dati."};

    std::vector<char> current_block;
    size_t block_pos = 0;
    std::map<std::string, int> flat_names_counter;

    // Traccia i file estratti per risolvere i duplicati
    std::map<uint32_t, std::string> extracted_paths;

    for (size_t i = 0; i < toc.size(); ++i) {
        auto& fe = toc[i];

        bool should_extract = patterns.empty();
        if (!should_extract) {
            for (const auto& pat : patterns) {
                if (match_pattern(fe.name, pat)) {
                    should_extract = true;
                    break;
                }
            }
        }

        // ── DUPLICATI: nessun dato nel flusso chunk ────────────────────────────
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

                UI::print_progress(i + 1, toc.size(), fe.name);

                if (!test_only) {
                    // Cerca il file originale gia' estratto e copialo
                    auto it = extracted_paths.find(fe.meta.duplicate_of_idx);
                    if (it != extracted_paths.end()) {
                        try {
                            fs::copy_file(it->second, final_path, fs::copy_options::overwrite_existing);
                        } catch (...) {
                            UI::print_warning("Impossibile copiare duplicato: " + fe.name);
                        }
                    } else {
                        UI::print_warning("Originale non estratto per duplicato: " + fe.name);
                    }
                } else {
                    // Test: il duplicato ha lo stesso hash dell'originale
                    UI::print_extract(fe.name, fe.meta.orig_size, true, true);
                }
                result.bytes_out += fe.meta.orig_size;
                extracted_paths[static_cast<uint32_t>(i)] = final_path;
            }
            continue;
        }

        // ── NON-DUPLICATO: leggi dati dal flusso chunk ─────────────────────────
        // Intervento #4: gestisce file che spannano piu' chunk
        std::vector<char> file_data;
        size_t remaining = fe.meta.orig_size;
        size_t src_pos = block_pos;

        while (remaining > 0) {
            // Se abbiamo consumato il chunk corrente, leggi il prossimo
            if (src_pos >= current_block.size()) {
                DecodedChunk chunk = read_next_chunk(f);
                if (!chunk.valid) {
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

        // Gestione percorso flat
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

        if (!test_only) {
            IO::write_file_to_disk(final_path, file_data.data(), file_data.size(), fe.meta.timestamp);
        }

        // Verifica XXH64 in modalita' test
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
        extracted_paths[static_cast<uint32_t>(i)] = final_path;
    }

    return result;
}

TarcResult list(const std::string& arch_path, size_t offset) {
    TarcResult res;
    FilePtr f(fopen(arch_path.c_str(), "rb"));
    if (!f) return {false, "Errore apertura archivio."};
    if (offset > 0) {
        if (!IO::seek64(f, static_cast<int64_t>(offset), SEEK_SET))
            return {false, "Errore seek."};
    }
    ::Header h;
    if (fread(&h, sizeof(h), 1, f) != 1) return {false, "Errore Header"};

    std::vector<::FileEntry> toc;
    h.toc_offset += offset;
    if (!IO::read_toc(f, h, toc)) return {false, "Errore TOC"};

    for (const auto& fe : toc)
        UI::print_list_entry(fe.name, fe.meta.orig_size, fe.meta.is_duplicate ? 0 : 1, static_cast<Codec>(fe.meta.codec));
    res.ok = true;
    return res;
}

TarcResult remove_files(const std::string&, const std::vector<std::string>&) {
    return {false, "Rimozione non supportata in modalita' Solid senza riscrittura completa."};
}

} // namespace Engine
