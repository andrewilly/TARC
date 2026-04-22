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

// ─── HELP (Stile UPX 5.1.1) ──────────────────────────────────────────────────
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
    printf("  %s-l%s           Elenca contenuto (Visualizza dettagli Solid)\n", G, R);
    printf("  %s-t%s           Test integrità (Verifica XXH64 Hardware)\n", Y, R);

    printf("\nCompression Levels:\n");
    printf("  %s-cbest%s       Livello Massimo (LZMA 256MB Chunk)\n", G, R);
    printf("  %s-cfast%s       Velocità Massima (LZ4 / ZSTD Fast)\n", G, R);

    printf("\nOptions:\n");
    printf("  %s--sfx%s        Genera archivio Autoestraente (.exe)\n", W, R);
    printf("  %s--flat%s       Estrazione Flat: ignora percorsi, file nella cartella corrente\n", W, R);

    printf("\nFeatures:\n");
    printf("  Solid Blocks 256MB, Deduplicazione XXH64, Win32 Native IO, Filtri Avanzati\n\n");

    printf("Type 'tarc --help' for more detailed help.\n");
    printf("%sTARC comes with ABSOLUTELY NO WARRANTY.%s\n\n", D, R);
}

// ─── BANNER (Stile Professionale - Credits: André Willy Rizzo) ──────────────
void show_banner() {
    printf("%s========================================================================\n", Color::CYAN);
    printf("TARC STRIKE v2.00             Advanced Solid Compression\n");
    printf("Copyright (C) 2026            André Willy Rizzo\n");
    printf("========================================================================%s\n", Color::RESET);
    // Nota: La licenza non viene stampata qui per evitare duplicati con il main
}

// ─── PROGRESS BAR ───────────────────────────────────────────────────────────
void print_progress(size_t current, size_t total, const std::string& current_file) {
    // Protezione contro divisione per zero
    float percent = (total > 0) ? ((float)current / total * 100.0f) : 100.0f;
    int width = 25;
    int pos = (total > 0) ? (int)(width * current / total) : width;

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

// Sostituisci la funzione compress_ratio in ui.cpp con questa versione migliorata:
std::string compress_ratio(uint64_t orig, uint64_t comp) {
    if (orig == 0) return "  -  ";
    char buf[32];
    double ratio = 100.0 * (1.0 - (double)comp / (double)orig);
    if (ratio < 0) ratio = 0;
    if (ratio > 99.9) snprintf(buf, sizeof(buf), "%.1f%%", ratio);
    else snprintf(buf, sizeof(buf), "%.2f%%", ratio);
    return std::string(buf);
}

// ─── PRINT OPERATIONS ────────────────────────────────────────────────────────
void print_add(const std::string& name, uint64_t size, Codec codec, float ratio) {
    // Nota: ratio >= 1.0 nel codice precedente indicava deduplicazione basata su logica custom
    bool is_dedup = (ratio >= 1.0f); 
    
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
    std::cout << std::endl; 
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

void print_info(const std::string& msg) {
    printf("%sℹ  INFO: %s%s\n", Color::CYAN, msg.c_str(), Color::RESET);
}

void print_error(const std::string& msg) {
    printf("%s❌ ERROR: %s%s\n", Color::RED, msg.c_str(), Color::RESET);
}

void print_warning(const std::string& msg) {
    printf("%s⚠  WARNING: %s%s\n", Color::YELLOW, msg.c_str(), Color::RESET);
}

} // namespace UI
