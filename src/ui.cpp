#include "ui.h"
#include <iostream>
#include "types.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <iomanip>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#endif

namespace UI {

// ─── INTERVENTO #16: VERBOSE FLAG GLOBALE ─────────────────────────────────────
bool g_verbose = false;

// ─── INTERVENTO #17: TIMER PER ETA ─────────────────────────────────────────────
static std::chrono::steady_clock::time_point g_progress_start;

void progress_timer_reset() {
    g_progress_start = std::chrono::steady_clock::now();
}

// ─── VTP (Virtual Terminal Processing) ───────────────────────────────────────
void enable_vtp() {
#ifdef _WIN32
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE) {
        DWORD dwMode = 0;
        if (GetConsoleMode(hOut, &dwMode)) {
            dwMode |= 0x0004; // ENABLE_VIRTUAL_TERMINAL_PROCESSING
            SetConsoleMode(hOut, dwMode);
        }
    }
    SetConsoleOutputCP(65001); // UTF-8
#endif
}

// ─── HELP ─────────────────────────────────────────────────────────────────────
void show_help() {
    const char* C = Color::CYAN;
    const char* R = Color::RESET;
    const char* W = Color::WHITE;
    const char* G = Color::GREEN;
    const char* Y = Color::YELLOW;
    const char* D = Color::DIM;

    printf("Usage: tarc [%s-cxlta%s] [%s-cbest|cfast%s] [%s--sfx%s] [%s--exclude%s pat] "
           "[%s-o%s dir] [%s-v%s] %sarchive [file..]%s\n\n",
            G, R, G, R, W, R, W, R, W, R, G, R, Y, R);

    printf("Commands:\n");
    printf("  %s-c / -a%s      Crea o Aggiorna Archivio Solid (Deduplicazione ON)\n", G, R);
    printf("  %s-x [filter]%s   Estrai file (Supporta wildcard es. *.txt)\n", C, R);
    printf("  %s-l%s           Elenca contenuto (visualizza dettagli Solid)\n", G, R);
    printf("  %s-t%s           Test integrita' (Verifica XXH64 + Chunk Checksum)\n", Y, R);
    printf("  %s-r [filter]%s   Rimuovi file dall'archivio (riscrittura completa)\n", C, R);

    printf("\nCompression Levels:\n");
    printf("  %s-cfast%s       Velocita' Massima (LZ4)\n", G, R);
    printf("  %s-c%s           Bilanciato (ZSTD piccoli / LZMA grandi)\n", G, R);
    printf("  %s-cbest%s       Massima Compressione (LZMA Extreme)\n", G, R);

    printf("\nOptions:\n");
    printf("  %s--sfx%s        Genera archivio Autoestraente (.exe)\n", W, R);
    printf("  %s--flat%s       Estrazione Flat: ignora percorsi, file nella cartella corrente\n", W, R);
    printf("  %s-o <dir>%s     Estrai nella directory specificata (crea se non esiste)\n", W, R);
    printf("  %s--exclude%s    Escludi file corrispondenti al pattern dalla compressione\n", W, R);
    printf("  %s-v%s           Modalita' verbose: output dettagliato\n", W, R);

    printf("\nCodecs: ZSTD | LZMA | LZ4 | Brotli | STORE (auto-selezione)\n");
    printf("Security: Path Traversal Protection, Atomic Writes, Header Validation\n\n");

    printf("Type 'tarc --help' for more detailed help.\n");
    printf("%sTARC comes with ABSOLUTELY NO WARRANTY.%s\n\n", D, R);
}

// ─── BANNER ──────────────────────────────────────────────────────────────────
void show_banner() {
    printf("%s========================================================================\n", Color::CYAN);
    printf("TARC STRIKE v2.04             Advanced Solid Compression\n");
    printf("Copyright (C) 2026            Andre Willy Rizzo\n");
    printf("========================================================================%s\n", Color::RESET);
}

// ─── INTERVENTO #17: PROGRESS BAR CON ETA ─────────────────────────────────────
void print_progress(size_t current, size_t total, const std::string& current_file) {
    float percent = (total > 0) ? (static_cast<float>(current) / total * 100.0f) : 100.0f;
    int width = 25;
    int pos = (total > 0) ? (static_cast<int>(width * current / total)) : width;

    std::string short_name = current_file;
    if (short_name.length() > 20) short_name = "..." + short_name.substr(short_name.length() - 17);

    // Calcola ETA
    char eta_buf[32] = "";
    if (current > 1 && total > 0) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - g_progress_start).count();
        if (elapsed > 0) {
            float rate = static_cast<float>(current) / static_cast<float>(elapsed);
            int remaining_sec = static_cast<int>((total - current) / rate);
            if (remaining_sec >= 3600) {
                snprintf(eta_buf, sizeof(eta_buf), " ETA %dh%02dm", remaining_sec / 3600, (remaining_sec % 3600) / 60);
            } else if (remaining_sec >= 60) {
                snprintf(eta_buf, sizeof(eta_buf), " ETA %dm%02ds", remaining_sec / 60, remaining_sec % 60);
            } else {
                snprintf(eta_buf, sizeof(eta_buf), " ETA %ds", remaining_sec);
            }
        }
    }

    printf("\r%s [%s", Color::CYAN, Color::BOLD);
    for (int i = 0; i < width; ++i) {
        if (i < pos) printf("=");
        else if (i == pos) printf(">");
        else printf(" ");
    }
    printf("%s%s] %.1f%% %sProcessing: %s%-20s%s",
           Color::RESET, Color::CYAN,
           percent,
           Color::DIM, Color::RESET, short_name.c_str(), eta_buf);
    fflush(stdout);
}

// ─── UTILITIES ───────────────────────────────────────────────────────────────
std::string human_size(uint64_t b) {
    char buf[32];
    if      (b > 1073741824ULL) snprintf(buf, sizeof(buf), "%.2f GB", b / 1073741824.0);
    else if (b > 1048576ULL)    snprintf(buf, sizeof(buf), "%.2f MB", b / 1048576.0);
    else if (b > 1024ULL)       snprintf(buf, sizeof(buf), "%.2f KB", b / 1024.0);
    else                        snprintf(buf, sizeof(buf), "%llu B",  (unsigned long long)b);
    return std::string(buf);
}

std::string compress_ratio(uint64_t orig, uint64_t comp) {
    if (orig == 0) return "  -  ";
    char buf[16];
    double r = 100.0 * (1.0 - static_cast<double>(comp) / static_cast<double>(orig));
    if (r < 0) r = 0;
    snprintf(buf, sizeof(buf), "%.1f%%", r);
    return std::string(buf);
}

// ─── PRINT OPERATIONS ────────────────────────────────────────────────────────
void print_add(const std::string& name, uint64_t size, Codec codec, float ratio) {
    bool is_dedup = (ratio >= 1.0f);

    printf("\n%s[+]%s [%s%s%s] %-38s %10s  %s->%s %s%s",
            Color::GREEN, Color::RESET,
            Color::YELLOW, codec_name(codec), Color::RESET,
            name.c_str(),
            human_size(size).c_str(),
            Color::DIM, Color::RESET,
            is_dedup ? Color::CYAN : "",
            is_dedup ? "DEDUPLICATED" : compress_ratio(size, static_cast<uint64_t>(size * (1.0f - ratio))).c_str());
}

void print_extract(const std::string& name, uint64_t size, bool test, bool ok) {
    if (!ok) {
        printf("%s[CORROTTO]%s %s\n", Color::RED, Color::RESET, name.c_str());
        return;
    }
    printf("%s[%s]%s %-42s %10s\n",
            Color::CYAN,
            test ? "OK" : " x",
            Color::RESET,
            name.c_str(),
            human_size(size).c_str());
}

void print_delete(const std::string& name) {
    printf("%s[-]%s Rimosso: %s\n", Color::RED, Color::RESET, name.c_str());
}

void print_list_entry(const std::string& name, uint64_t orig, uint64_t comp, Codec codec) {
    bool is_duplicate = (comp == 0);

    printf("  [%s%s%s] %-42s %10s  %s%s%s\n",
            Color::YELLOW, codec_name(codec), Color::RESET,
            name.c_str(),
            human_size(orig).c_str(),
            Color::DIM,
            is_duplicate ? "(DUPLICATE)" : "",
            Color::RESET);
}

// ─── INTERVENTO #18: SUMMARY ARRICCHITO ────────────────────────────────────────
void print_summary(const TarcResult& r, const std::string& op) {
    printf("\n");
    if (!r.ok) {
        printf("\n%sX %s fallito: %s%s\n", Color::RED, op.c_str(), r.message.c_str(), Color::RESET);
        return;
    }

    // Operazione completata
    printf("\n%s+ %s completato.%s", Color::GREEN, op.c_str(), Color::RESET);

    // Statistiche byte se disponibili
    if (r.bytes_in > 0 && r.bytes_out > 0) {
        printf("  %s -> %s  (%sratio: %s%s)",
                human_size(r.bytes_in).c_str(),
                human_size(r.bytes_out).c_str(),
                Color::DIM,
                compress_ratio(r.bytes_in, r.bytes_out).c_str(),
                Color::RESET);
    }
    printf("\n");

    // Conteggi file
    if (r.file_count > 0) {
        printf("  %sFile:%s %u", Color::DIM, Color::RESET, r.file_count);
        if (r.dup_count > 0)
            printf("  %sDuplicati:%s %u", Color::CYAN, Color::RESET, r.dup_count);
        if (r.skip_count > 0)
            printf("  %sSaltati:%s %u", Color::YELLOW, Color::RESET, r.skip_count);
        printf("\n");
    }

    // Statistiche per-codec
    if (!r.codec_bytes.empty()) {
        printf("  %sCodecs:%s", Color::DIM, Color::RESET);
        for (const auto& [codec, bytes] : r.codec_bytes) {
            auto chunk_it = r.codec_chunks.find(codec);
            uint32_t chunks = (chunk_it != r.codec_chunks.end()) ? chunk_it->second : 0;
            printf(" %s%s%s %s(%u chunk%s)",
                   Color::YELLOW, codec_name(codec), Color::RESET,
                   human_size(bytes).c_str(),
                   chunks,
                   chunks != 1 ? "s" : "");
        }
        printf("\n");
    }

    // Tempo impiegato
    if (r.elapsed_ms > 0) {
        printf("  %sTempo:%s ", Color::DIM, Color::RESET);
        if (r.elapsed_ms >= 60000) {
            printf("%.1f min", r.elapsed_ms / 60000.0);
        } else if (r.elapsed_ms >= 1000) {
            printf("%.2f sec", r.elapsed_ms / 1000.0);
        } else {
            printf("%llu ms", (unsigned long long)r.elapsed_ms);
        }

        // Velocita' se abbiamo bytes e tempo
        if (r.bytes_in > 0 && r.elapsed_ms > 0) {
            double mb_per_sec = (r.bytes_in / 1048576.0) / (r.elapsed_ms / 1000.0);
            printf("  %s(%.1f MB/s)%s", Color::DIM, mb_per_sec, Color::RESET);
        }
        printf("\n");
    }

    // Dimensione archivio
    if (r.archive_size > 0) {
        printf("  %sArchivio:%s %s\n", Color::DIM, Color::RESET, human_size(r.archive_size).c_str());
    }
}

void print_info(const std::string& msg) {
    printf("%sINFO: %s%s\n", Color::CYAN, msg.c_str(), Color::RESET);
}

void print_error(const std::string& msg) {
    printf("%sERROR: %s%s\n", Color::RED, msg.c_str(), Color::RESET);
}

void print_warning(const std::string& msg) {
    printf("%sWARNING: %s%s\n", Color::YELLOW, msg.c_str(), Color::RESET);
}

// ─── INTERVENTO #16: VERBOSE LOGGING ─────────────────────────────────────────
void print_verbose(const std::string& msg) {
    if (!g_verbose) return;
    printf("%sVERBOSE: %s%s\n", Color::DIM, msg.c_str(), Color::RESET);
}

} // namespace UI
