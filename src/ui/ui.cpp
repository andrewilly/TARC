#include "tarc/ui/ui.h"
#include <iostream>
#include <iomanip>
#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#endif

namespace tarc::ui {

namespace {
    constexpr const char* RESET  = "\x1b[0m";
    constexpr const char* BOLD   = "\x1b[1m";
    constexpr const char* CYAN   = "\x1b[38;5;81m";
    constexpr const char* GREEN  = "\x1b[32m";
    constexpr const char* YELLOW = "\x1b[33m";
    constexpr const char* RED    = "\x1b[31m";
    constexpr const char* DIM    = "\x1b[2m";
}

// ═══════════════════════════════════════════════════════════════════════════
// INITIALIZATION
// ═══════════════════════════════════════════════════════════════════════════

void enable_vtp() {
#ifdef _WIN32
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
        if (GetConsoleMode(hOut, &mode)) {
            mode |= 0x0004;  // ENABLE_VIRTUAL_TERMINAL_PROCESSING
            SetConsoleMode(hOut, mode);
        }
    }
    SetConsoleOutputCP(65001);  // UTF-8
#endif
}

void show_banner() {
    printf("%s========================================================================\n", CYAN);
    printf("TARC STRIKE v2.01c             Advanced Solid Compression\n");
    printf("Copyright (C) 2026            André Willy Rizzo\n");
    printf("========================================================================%s\n\n", RESET);
}

void show_help() {
    printf("Usage: tarc [%s-cxlta%s] [%soptions%s] %sarchive [files...]%s\n\n", 
           GREEN, RESET, GREEN, RESET, YELLOW, RESET);
    
    printf("Commands:\n");
    printf("  %s-c%s      Crea archivio\n", GREEN, RESET);
    printf("  %s-a%s      Append a archivio esistente\n", GREEN, RESET);
    printf("  %s-x%s      Estrai file\n", CYAN, RESET);
    printf("  %s-l%s      Lista contenuto\n", GREEN, RESET);
    printf("  %s-t%s      Test integrità\n", YELLOW, RESET);
    
    printf("\nOptions:\n");
    printf("  %s-cbest%s   Massima compressione\n", GREEN, RESET);
    printf("  %s-cfast%s   Compressione veloce\n", GREEN, RESET);
    printf("  %s--sfx%s    Crea eseguibile autoestraente\n", RESET, RESET);
    printf("  %s--flat%s   Estrai senza percorsi\n\n", RESET, RESET);
}

// ═══════════════════════════════════════════════════════════════════════════
// FORMATTING
// ═══════════════════════════════════════════════════════════════════════════

std::string human_size(uint64_t bytes) {
    char buf[32];
    if      (bytes >= 1073741824ULL) snprintf(buf, sizeof(buf), "%.2f GB", bytes / 1073741824.0);
    else if (bytes >= 1048576ULL)    snprintf(buf, sizeof(buf), "%.2f MB", bytes / 1048576.0);
    else if (bytes >= 1024ULL)       snprintf(buf, sizeof(buf), "%.2f KB", bytes / 1024.0);
    else                             snprintf(buf, sizeof(buf), "%llu B",  bytes);
    return std::string(buf);
}

std::string compress_ratio(uint64_t orig, uint64_t comp) {
    if (orig == 0) return "  -  ";
    char buf[16];
    double ratio = 100.0 * (1.0 - static_cast<double>(comp) / static_cast<double>(orig));
    snprintf(buf, sizeof(buf), "%.1f%%", std::max(0.0, ratio));
    return std::string(buf);
}

// ═══════════════════════════════════════════════════════════════════════════
// OUTPUT
// ═══════════════════════════════════════════════════════════════════════════

void print_info(const std::string& msg) {
    printf("%sℹ  INFO: %s%s\n", CYAN, msg.c_str(), RESET);
}

void print_error(const std::string& msg) {
    printf("%s❌ ERROR: %s%s\n", RED, msg.c_str(), RESET);
}

void print_warning(const std::string& msg) {
    printf("%s⚠  WARNING: %s%s\n", YELLOW, msg.c_str(), RESET);
}

void print_summary(const Result& result, const std::string& operation) {
    if (result.failed()) {
        printf("\n%s❌ %s fallito: %s%s\n", 
               RED, operation.c_str(), result.message.c_str(), RESET);
        return;
    }
    
    if (result.bytes_in > 0 && result.bytes_out > 0) {
        printf("\n%s✔ %s completato.%s  %s → %s  (ratio: %s)\n",
               GREEN, operation.c_str(), RESET,
               human_size(result.bytes_in).c_str(),
               human_size(result.bytes_out).c_str(),
               compress_ratio(result.bytes_in, result.bytes_out).c_str());
    } else {
        printf("\n%s✔ %s completato.%s\n", GREEN, operation.c_str(), RESET);
    }
}

void print_progress(size_t current, size_t total, const std::string& filename) {
    float percent = (total > 0) ? (static_cast<float>(current) / total * 100.0f) : 100.0f;
    int width = 25;
    int pos = (total > 0) ? static_cast<int>(width * current / total) : width;
    
    std::string short_name = filename;
    if (short_name.length() > 20) {
        short_name = "..." + short_name.substr(short_name.length() - 17);
    }
    
    printf("\r%s [%s", CYAN, BOLD);
    for (int i = 0; i < width; ++i) {
        if (i < pos) printf("=");
        else if (i == pos) printf(">");
        else printf(" ");
    }
    printf("%s%s] %.1f%% %s%s%s", RESET, CYAN, percent, DIM, short_name.c_str(), RESET);
    std::cout << std::flush;
}

} // namespace tarc::ui
