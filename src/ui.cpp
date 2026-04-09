#include "ui.h"
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

namespace UI {

// ─── VTP ─────────────────────────────────────────────────────────────────────
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
    const char* B = Color::BOLD;
    const char* W = Color::WHITE;
    const char* G = Color::GREEN;
    const char* Y = Color::YELLOW;
    const char* D = Color::RED;

    printf("\n");
    printf("  TARC v1.03 - HYBRID COMPRESSION ENGINE\n");
    printf("  ======================================\n\n");
    
    printf("  %s%-12s%s %s- %sCrea archivio       (Livello 1-22, def: 3)%s\n", 
           G, "-c[N]", R, G, W, R);
    printf("  %s%-12s%s %s- %sAggiungi file       (Livello 1-22)%s\n", 
           Y, "-a[N]", R, Y, W, R);
    printf("  %s%-12s%s %s- %sEstrai tutto%s\n", 
           G, "-x", R, G, W, R);
    printf("  %s%-12s%s %s- %sElenca contenuto%s\n", 
           G, "-l", R, G, W, R);
    printf("  %s%-12s%s %s- %sTest integrita      (XXH64)%s\n", 
           Y, "-t", R, Y, W, R);
    printf("  %s%-12s%s %s- %sElimina file        (Wildcards supportati)%s\n", 
           D, "-d", R, D, W, R);
    
    printf("\n  Codec automatico: LZ4 (binari), ZSTD (generico), LZMA (testo)\n\n");
}

// ─── BANNER ──────────────────────────────────────────────────────────────────
void show_banner() {
    printf("%s%s  ████████╗ █████╗ ██████╗  ██████╗ %s\n", Color::BOLD, Color::CYAN, Color::RESET);
    printf("%s%s  ╚══██╔══╝██╔══██╗██╔══██╗██╔════╝ %s\n", Color::BOLD, Color::CYAN, Color::RESET);
    printf("%s%s     ██║   ███████║██████╔╝██║      %s\n", Color::BOLD, Color::CYAN, Color::RESET);
    printf("%s%s     ██║   ██╔══██║██╔══██╗██║      %s\n", Color::BOLD, Color::CYAN, Color::RESET);
    printf("%s%s     ██║   ██║  ██║██║  ██║╚██████╗ %s\n", Color::BOLD, Color::CYAN, Color::RESET);
    printf("%s%s     ╚═╝   ╚═╝  ╚═╝╚═╝  ╚═╝ ╚═════╝ %sv1.03\n\n", Color::BOLD, Color::DIM, Color::RESET);
}

// ─── UTILITIES ───────────────────────────────────────────────────────────────
std::string human_size(uint64_t b) {
    char buf[32];
    if      (b > 1073741824ULL) snprintf(buf, sizeof(buf), "%.2f GB", b / 1073741824.0);
    else if (b > 1048576ULL)    snprintf(buf, sizeof(buf), "%.2f MB", b / 1048576.0);
    else if (b > 1024ULL)       snprintf(buf, sizeof(buf), "%.2f KB", b / 1024.0);
    else                        snprintf(buf, sizeof(buf), "%llu B",  (unsigned long long)b);
    return buf;
}

std::string compress_ratio(uint64_t orig, uint64_t comp) {
    if (orig == 0) return "  -  ";
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1f%%", 100.0 * (1.0 - (double)comp / (double)orig));
    return buf;
}

// ─── PRINT OPERATIONS ────────────────────────────────────────────────────────
void print_add(const std::string& name, uint64_t size, Codec codec, float ratio) {
    char ratio_str[16];
    snprintf(ratio_str, sizeof(ratio_str), "%.1f%%", ratio * 100.0f);

    printf("%s[+]%s [%s%s%s] %-38s %10s  %s→%s %s\n",
           Color::GREEN, Color::RESET,
           Color::YELLOW, codec_name(codec), Color::RESET,
           name.c_str(),
           human_size(size).c_str(),
           Color::DIM, Color::RESET,
           ratio_str);
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
    printf("  [%s%s%s] %-42s %10s  %s(%s)%s\n",
           Color::YELLOW, codec_name(codec), Color::RESET,
           name.c_str(),
           human_size(orig).c_str(),
           Color::DIM, compress_ratio(orig, comp).c_str(), Color::RESET);
}

void print_summary(const TarcResult& r, const std::string& op) {
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
    printf("%s❌ %s%s\n", Color::RED, msg.c_str(), Color::RESET);
}

void print_warning(const std::string& msg) {
    printf("%s⚠  %s%s\n", Color::YELLOW, msg.c_str(), Color::RESET);
}

} // namespace UI
