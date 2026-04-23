#include "ui.h"
#include <iostream>
#include "types.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <iomanip>
#include <chrono>
#include <cmath>

#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif

namespace Color {
    const char* RED     = "\x1b[31m";
    const char* GREEN   = "\x1b[32m";
    const char* YELLOW  = "\x1b[33m";
    const char* BLUE    = "\x1b[34m";
    const char* MAGENTA = "\x1b[35m";
    const char* CYAN    = "\x1b[36m";
    const char* WHITE   = "\x1b[37m";
    const char* RESET   = "\x1b[0m";
    const char* BOLD    = "\x1b[1m";
    const char* DIM     = "\x1b[2m";
}

namespace UI {

bool g_verbose = false;
static std::chrono::steady_clock::time_point g_progress_start;

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
    SetConsoleOutputCP(65001);
#endif
}

void show_banner() {
    printf("%s%s", Color::BOLD, Color::CYAN);
    printf("┌────────────────────────────────────────────────────────┐\n");
    printf("│  TARC Archiver v2.04 - High Performance Refactored     │\n");
    printf("└────────────────────────────────────────────────────────┘\n");
    printf("%s", Color::RESET);
}

void show_help() {
    printf("\n%sUtilizzo:%s tarc [comando] [archivio] [file/pattern...]\n", Color::BOLD, Color::RESET);
    printf("\n%sComandi:%s\n", Color::BOLD, Color::RESET);
    printf("  -c <file>   Crea nuovo archivio\n");
    printf("  -a <file>   Aggiungi file ad archivio esistente\n");
    printf("  -x <file>   Estrai tutto il contenuto\n");
    printf("  -l <file>   Elenca file nell'archivio\n");
    printf("  -r <file>   Rimuovi file (supporta wildcards)\n");
    printf("\n%sOpzioni:%s\n", Color::BOLD, Color::RESET);
    printf("  -c1..-c9    Livello compressione (default 3)\n");
    printf("  -v          Modalità dettagliata (verbose)\n");
    printf("  -o <dir>    Directory di output per estrazione\n");
}

void print_info(const std::string& msg)    { printf("%sℹ%s %s\n", Color::CYAN, Color::RESET, msg.c_str()); }
void print_success(const std::string& msg) { printf("%s✔%s %s\n", Color::GREEN, Color::RESET, msg.c_str()); }
void print_warning(const std::string& msg) { printf("%s⚠%s %s%s%s\n", Color::YELLOW, Color::RESET, Color::YELLOW, msg.c_str(), Color::RESET); }
void print_error(const std::string& msg)   { printf("%s✖%s %s%s%s\n", Color::RED, Color::RESET, Color::BOLD, msg.c_str(), Color::RESET); }
void print_verbose(const std::string& msg) { if (g_verbose) printf("%s[debug]%s %s\n", Color::DIM, Color::RESET, msg.c_str()); }

void progress_timer_reset() { g_progress_start = std::chrono::steady_clock::now(); }

void update_progress(uint64_t current, uint64_t total, const std::string& label) {
    if (total == 0) return;
    float perc = (float)current / total;
    int width = 30;
    int pos = (int)(width * perc);

    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_progress_start).count();
    
    double mb_per_sec = 0;
    if (ms > 0) mb_per_sec = (current / 1048576.0) / (ms / 1000.0);

    printf("\r%s%-15.15s%s [", Color::CYAN, label.c_str(), Color::RESET);
    for (int i = 0; i < width; ++i) {
        if (i < pos) printf("■");
        else if (i == pos) printf("▶");
        else printf(" ");
    }
    printf("] %3d%%  %s%.1f MB/s%s   ", (int)(perc * 100), Color::DIM, mb_per_sec, Color::RESET);
    fflush(stdout);
    if (current >= total) printf("\n");
}

std::string human_size(uint64_t bytes) {
    const char* suffix[] = {"B", "KB", "MB", "GB", "TB"};
    int i = 0;
    double dblBytes = (double)bytes;
    if (bytes > 1024) {
        for (i = 0; (bytes / 1024) > 0 && i < 4; i++, bytes /= 1024) dblBytes = bytes / 1024.0;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "%.2f %s", dblBytes, suffix[i]);
    return std::string(buf);
}

void print_summary(const TarcResult& r, const std::string& title) {
    printf("\n%s┌────────────────────────────────────────────────────────┐\n", Color::CYAN);
    printf("│ RIASSUNTO OPERAZIONE: %-32.32s │\n", title.c_str());
    printf("├────────────────────────────────────────────────────────┤\n");
    printf("│ %-20s : %31s │\n", "Stato", r.ok ? "SUCCESSO" : "FALLITO");
    printf("│ %-20s : %31s │\n", "File elaborati", std::to_string(r.files_proc).c_str());
    printf("│ %-20s : %31s │\n", "Dimensione Input", human_size(r.bytes_in).c_str());
    printf("│ %-20s : %31s │\n", "Dimensione Output", human_size(r.bytes_out).c_str());
    
    if (r.bytes_in > 0) {
        double ratio = (1.0 - (double)r.bytes_out / r.bytes_in) * 100.0;
        printf("│ %-20s : %30.1f%% │\n", "Ratio compressione", ratio);
    }

    if (r.elapsed_ms > 0) {
        double sec = r.elapsed_ms / 1000.0;
        char time_buf[32];
        snprintf(time_buf, sizeof(time_buf), "%.2f secondi", sec);
        printf("│ %-20s : %31s │\n", "Tempo impiegato", time_buf);
    }
    printf("└────────────────────────────────────────────────────────┘%s\n", Color::RESET);
}

} // namespace UI