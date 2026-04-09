#include "ui.h"
#include "types.h" // Inserito per leggere TARC_VERSION automaticamente
#include <iostream>
#include <cstdio>
#include <vector>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace UI {

void enable_vtp() {
#ifdef _WIN32
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD dwMode = 0;
    GetConsoleMode(hOut, &dwMode);
    dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(hOut, dwMode);
#endif
}

void show_banner() {
    // Utilizza TARC_VERSION definita in types.h per automatizzare il numero versione
    printf("%sTARC Archiver %s- v%d.0%d%s\n", Color::CYAN, Color::BOLD, 1, TARC_VERSION % 100, Color::RESET);
    printf("Compressione Multi-Algoritmo (ZSTD, LZ4, LZMA)\n\n");
}

void show_help() {
    const char* G = Color::GREEN;
    const char* R = Color::RESET;
    const char* Y = Color::YELLOW;

    printf("%sUTILIZZO:%s\n", Y, R);
    printf("  tarc <comando> <archivio> [file...]\n\n");

    printf("%sCOMANDI:%s\n", Y, R);
    printf("  %s-c[1-22]%s  Crea nuovo archivio (es: -c9 per livello 9)\n", G, R);
    printf("  %s-cbest%s    Livello massimo: compressione ultra (LZMA/ZSTD 22)\n", G, R);
    printf("  %s-cfast%s    Livello minimo: massima velocità (LZ4)\n", G, R);
    printf("  %s-a%s        Smart Update: aggiunge o aggiorna solo i file modificati\n", G, R);
    printf("  %s-x%s        Estrae tutti i file\n", G, R);
    printf("  %s-t%s        Test di integrità dell'archivio\n", G, R);
    printf("  %s-l%s        Elenca il contenuto\n", G, R);
    printf("  %s-d%s        Elimina file dall'archivio\n\n", G, R);

    printf("%sESEMPI:%s\n", Y, R);
    printf("  tarc -cbest backup.tarc documenti/*.pdf\n");
    printf("  tarc -a backup.tarc cartella_lavoro/\n");
}

void print_add(const std::string& name, uint64_t size, Codec codec, float ratio) {
    const char* c_name = "NONE";
    if (codec == Codec::ZSTD) c_name = "ZSTD";
    else if (codec == Codec::LZ4)  c_name = "LZ4 ";
    else if (codec == Codec::LZMA) c_name = "LZMA";

    printf("  %s+ %-30s %s [%-4s] %s%3.0f%%%s\n", 
           Color::GREEN, name.c_str(), Color::CYAN, c_name, 
           Color::BOLD, ratio * 100.0f, Color::RESET);
}

void print_extract(const std::string& name, uint64_t size, bool test, bool ok) {
    if (test) {
        printf("  %s[%s]%s %-35s %s\n", 
               ok ? Color::GREEN : Color::RED, 
               ok ? " OK " : "FAIL", 
               Color::RESET, name.c_str(), ok ? "" : "(Errore Hash!)");
    } else {
        printf("  %s> %-35s %sOK%s\n", Color::CYAN, name.c_str(), Color::GREEN, Color::RESET);
    }
}

void print_list_entry(const std::string& name, uint64_t orig, uint64_t comp, Codec codec) {
    float r = orig > 0 ? (1.0f - (float)comp / orig) * 100.0f : 0;
    printf("  %-35s %10llu -> %10llu [%2.0f%%]\n", name.c_str(), orig, comp, r);
}

void print_summary(const TarcResult& res, const std::string& op) {
    if (res.ok) {
        printf("\n%s%s completata con successo.%s\n", Color::GREEN, op.c_str(), Color::RESET);
    } else {
        printf("\n%sErrore durante %s: %s%s\n", Color::RED, op.c_str(), res.message.c_str(), Color::RESET);
    }
}

void print_error(const std::string& msg) {
    printf("%sERRORE: %s%s\n", Color::RED, msg.c_str(), Color::RESET);
}

void print_warning(const std::string& msg) {
    printf("%sATTENZIONE: %s%s\n", Color::YELLOW, msg.c_str(), Color::RESET);
}

} // namespace UI
