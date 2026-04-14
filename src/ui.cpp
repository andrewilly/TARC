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
            dwMode |= 0x0004;
            SetConsoleMode(hOut, dwMode);
        }
    }
    SetConsoleOutputCP(65001);
#endif
}

// ─── HELP ─────────────────────────────────────────────────────────────────────
void show_help() {
    const char* C = Color::CYAN;
    const char* R = Color::RESET;
    const char* W = Color::WHITE;
    const char* G = Color::GREEN;
    const char* Y = Color::YELLOW;

    printf("\n");
    printf("  TARC v2.00 - HYBRID SOLID ENGINE\n");
    printf("  ======================================\n\n");
    
    printf("  %s%-12s%s %s- %sCrea/Aggiorna Solid   (Deduplicazione ON)%s\n", G, "-c / -a", R, G, W, R);
    printf("  %s%-12s%s %s- %sLivello Massimo      (LZMA 128MB Dict)%s\n", G, "-cbest", R, G, W, R);
    printf("  %s%-12s%s %s- %sVelocita Massima     (LZ4 / ZSTD Fast)%s\n", G, "-cfast", R, G, W, R);
    printf("  %s%-12s%s %s- %sEstrai tutto         (Ripristino percorsi)%s\n", C, "-x", R, C, W, R);
    printf("  %s%-12s%s %s- %sElenca contenuto      (Dettagli solid)%s\n", G, "-l", R, G, W, R);
    printf("  %s%-12s%s %s- %sTest integrita       (XXH64 Hardware)%s\n", Y, "-t", R, Y, W, R);
    
    printf("\n  Caratteristiche: Solid Blocks, Deduplicazione XXH64, Multi-threading\n\n");
}

// ─── BANNER ──────────────────────────────────────────────────────────────────
void show_banner() {
    std::cout << Color::CYAN << "==========================================\n";
    std::cout << "          TARC STRIKE v2.00               \n";
    std::cout << "      Advanced Solid Compression          \n";
    std::cout << "==========================================\n" << Color::RESET << std::endl;
}

// ─── PROGRESS BAR (NEW) ─────────────────────────────────────────────────────
void print_progress(size_t current, size_t total, const std::string& current_file) {
    float percent = (float)current / total * 100.0f;
    int width = 25;
    int pos = (int)(width * current / total);

    std::string short_name = current_file;
    if (short_name.length() > 20) short_name = "..." + short_name.substr(short_name.length() - 17);

    std::cout << "\r" << Color::CYAN << " [" << Color::BOLD;
    for (int i = 0; i < width; ++i) {
        if (i < pos) std::cout << "=";
        else if (i == pos) std::cout << ">";
        else std::cout << " ";
    }
    std::cout << Color::RESET << Color::CYAN << "] " 
              << std::fixed << std::setprecision(1) << percent << "% "
              << Color::DIM << "Processing: " << Color::RESET << std::left << std::setw(20) << short_name << std::flush;
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
    double r = 100.0 * (1.0 - (double)comp / (double)orig);
    if (r < 0) r = 0; // Evita ratio negativi se il file cresce
    snprintf(buf, sizeof(buf), "%.1f%%", r);
    return std::string(buf);
}

// ─── PRINT OPERATIONS ────────────────────────────────────────────────────────
void print_add(const std::string& name, uint64_t size, Codec codec, float ratio) {
    bool is_dedup = (ratio >= 1.0f); // Se ratio è 1.0, il file è un duplicato esatto
    
    printf("\n%s[+]%s [%s%s%s] %-38s %10s  %s→%s %s%s",
            Color::GREEN, Color::RESET,
            Color::YELLOW, codec_name(codec), Color::RESET,
            name.c_str(),
            human_size(size).c_str(),
            Color::DIM, Color::RESET,
            is_dedup ? Color::CYAN : "",
            is_dedup ? "DEDUPLICATED" : compress_ratio(size, (uint64_t)(size * (1.0f-ratio))).c_str());
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
    // Ora marchiamo DUPLICATE solo se è esplicitamente un duplicato (comp == 0)
    // Se comp è 1 (il trucco che ti ho suggerito), mostriamo il file come normale
    bool is_duplicate = (comp == 0); 

    printf("  [%s%s%s] %-42s %10s  %s%s%s\n",
            Color::YELLOW, codec_name(codec), Color::RESET,
            name.c_str(),
            human_size(orig).c_str(),
            Color::DIM, 
            is_duplicate ? "(DUPLICATE)" : "", // Mostra DUPLICATE solo se lo è davvero
            Color::RESET);
}

void print_summary(const TarcResult& r, const std::string& op) {
    std::cout << std::endl; // Pulisce la riga del progresso
    if (!r.ok) {
        printf("\n%s❌ %s fallito: %s%s\n", Color::RED, op.c_str(), r.message.c_str(), Color::RESET);
        return;
    }
    if (r.bytes_in > 0 && r.bytes_out > 0) {
        printf("\n%s✔ %s completato.%s  %s → %s  (%sratio: %s%s)\n",
                Color::GREEN, op.c_str(), Color::RESET,
                human_size(r.bytes_in).c_str(),
                human_size(r.bytes_out).c_str(),
                Color::DIM,
                compress_ratio(r.bytes_in, r.bytes_out).c_str(),
                Color::RESET);
    } else {
        printf("\n%s✔ %s completato.%s\n", Color::GREEN, op.c_str(), Color::RESET);
    }
}

void print_error(const std::string& msg) {
    printf("%s❌ ERROR: %s%s\n", Color::RED, msg.c_str(), Color::RESET);
}

void print_warning(const std::string& msg) {
    printf("%s⚠  WARNING: %s%s\n", Color::YELLOW, msg.c_str(), Color::RESET);
}

} // namespace UI
