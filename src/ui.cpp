#include "ui.h"
#include <iostream>
#include "types.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <iomanip>

#ifdef _WIN32
#include <windows.h>
#endif

namespace UI {

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

    printf("Usage: tarc [%s-cxlta%s] [%s-cbest|cfast%s] [%s--sfx%s] %sarchive [file..]%s\n\n",
            G, R, G, R, W, R, Y, R);

    printf("Commands:\n");
    printf("  %s-c / -a%s      Crea o Aggiorna Archivio Solid (Deduplicazione ON)\n", G, R);
    printf("  %s-x [filter]%s   Estrai file (Supporta wildcard es. *.txt)\n", C, R);
    printf("  %s-l%s           Elenca contenuto (visualizza dettagli Solid)\n", G, R);
    printf("  %s-t%s           Test integrita' (Verifica XXH64 + Chunk Checksum)\n", Y, R);

    printf("\nCompression Levels:\n");
    printf("  %s-cfast%s       Velocita' Massima (LZ4)\n", G, R);
    printf("  %s-c%s           Bilanciato (ZSTD piccoli / LZMA grandi)\n", G, R);
    printf("  %s-cbest%s       Massima Compressione (LZMA Extreme)\n", G, R);

    printf("\nOptions:\n");
    printf("  %s--sfx%s        Genera archivio Autoestraente (.exe)\n", W, R);
    printf("  %s--flat%s       Estrazione Flat: ignora percorsi, file nella cartella corrente\n", W, R);

    printf("\nCodecs: ZSTD | LZMA | LZ4 | Brotli | STORE (auto-selezione)\n");
    printf("Security: Path Traversal Protection, Atomic Writes, Header Validation\n\n");

    printf("Type 'tarc --help' for more detailed help.\n");
    printf("%sTARC comes with ABSOLUTELY NO WARRANTY.%s\n\n", D, R);
}

// ─── BANNER ──────────────────────────────────────────────────────────────────
void show_banner() {
    printf("%s========================================================================\n", Color::CYAN);
    printf("TARC STRIKE v2.02             Advanced Solid Compression\n");
    printf("Copyright (C) 2026            Andre Willy Rizzo\n");
    printf("========================================================================%s\n", Color::RESET);
}

// ─── PROGRESS BAR ───────────────────────────────────────────────────────────
void print_progress(size_t current, size_t total, const std::string& current_file) {
    float percent = (total > 0) ? (static_cast<float>(current) / total * 100.0f) : 100.0f;
    int width = 25;
    int pos = (total > 0) ? (static_cast<int>(width * current / total)) : width;

    std::string short_name = current_file;
    if (short_name.length() > 20) short_name = "..." + short_name.substr(short_name.length() - 17);

    printf("\r%s [%s", Color::CYAN, Color::BOLD);
    for (int i = 0; i < width; ++i) {
        if (i < pos) printf("=");
        else if (i == pos) printf(">");
        else printf(" ");
    }
    printf("%s%s] %.1f%% %sProcessing: %s%-20s",
           Color::RESET, Color::CYAN,
           percent,
           Color::DIM, Color::RESET, short_name.c_str());
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

void print_summary(const TarcResult& r, const std::string& op) {
    printf("\n");
    if (!r.ok) {
        printf("\n%sX %s fallito: %s%s\n", Color::RED, op.c_str(), r.message.c_str(), Color::RESET);
        return;
    }
    if (r.bytes_in > 0 && r.bytes_out > 0) {
        printf("\n%s+ %s completato.%s  %s -> %s  (%sratio: %s%s)\n",
                Color::GREEN, op.c_str(), Color::RESET,
                human_size(r.bytes_in).c_str(),
                human_size(r.bytes_out).c_str(),
                Color::DIM,
                compress_ratio(r.bytes_in, r.bytes_out).c_str(),
                Color::RESET);
    } else {
        printf("\n%s+ %s completato.%s\n", Color::GREEN, op.c_str(), Color::RESET);
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

} // namespace UI
