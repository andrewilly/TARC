#pragma once
#include <cstdint>
#include "types.h"

// ─── ANSI COLORS ──────────────────────────────────────────────────────────────
namespace Color {
    inline const char* RESET  = "\x1b[0m";
    inline const char* BOLD   = "\x1b[1m";
    inline const char* CYAN   = "\x1b[38;5;81m";
    inline const char* GREEN  = "\x1b[32m";
    inline const char* YELLOW = "\x1b[33m";
    inline const char* RED    = "\x1b[31m";
    inline const char* WHITE  = "\x1b[37m";
    inline const char* DIM    = "\x1b[2m";
}

// ─── UI FUNCTIONS ─────────────────────────────────────────────────────────────
namespace UI {

    // Abilita Virtual Terminal Processing su Windows
    void enable_vtp();

    // Stampa help dettagliato
    void show_help();

    // Stampa banner principale
    void show_banner();

    // Formatta byte in stringa leggibile (KB, MB, GB)
    std::string human_size(uint64_t bytes);

    // Formatta ratio di compressione
    std::string compress_ratio(uint64_t orig, uint64_t comp);

    // Stampa riga aggiunta/compressa
    void print_add(const std::string& name, uint64_t size, Codec codec, float ratio);

    // Stampa riga estratta/testata
    void print_extract(const std::string& name, uint64_t size, bool test, bool ok);

    // Stampa riga eliminata
    void print_delete(const std::string& name);

    // Stampa riga lista archivio
    void print_list_entry(const std::string& name, uint64_t orig, uint64_t comp, Codec codec);

    // Stampa riepilogo finale operazione
    void print_summary(const TarcResult& result, const std::string& operation);

    // Stampa errore
    void print_error(const std::string& msg);

    // Stampa warning
    void print_warning(const std::string& msg);

    // ... altre funzioni esistenti ...
    void print_progress(size_t current, size_t total, const std::string& current_file);

} // namespace UI
