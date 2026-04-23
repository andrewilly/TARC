#pragma once
#include <string>
#include <vector>
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

// ─── VERBOSE FLAG (Intervento #16) ─────────────────────────────────────────────
// Globale: impostato da main.cpp in base al flag -v
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

    // ─── INTERVENTO #17: PROGRESS BAR CON ETA ──────────────────────────────────
    // Barra di progresso con stima tempo rimanente
    // test_ok: -1 = non testando, 0 = FAIL, 1 = OK (mostra [OK]/[FAIL] inline)
    void print_progress(size_t current, size_t total, const std::string& current_file,
                        int test_ok = -1);

    // Resetta il timer interno per il calcolo ETA (chiamare prima di ogni operazione)
    void progress_timer_reset();

    // Stampa riga aggiunta/compressa
    void print_add(const std::string& name, uint64_t size, Codec codec, float ratio);

    // Stampa riga estratta/testata
    void print_extract(const std::string& name, uint64_t size, bool test, bool ok);

    // Stampa riga eliminata
    void print_delete(const std::string& name);

    // Stampa riga lista archivio
    void print_list_entry(const std::string& name, uint64_t orig, uint64_t comp, Codec codec);

    // ─── INTERVENTO #18: SUMMARY ARRICCHITO ────────────────────────────────────
    // Stampa riepilogo finale con statistiche per-codec, tempo, duplicati
    void print_summary(const TarcResult& result, const std::string& operation);

    // --- Messaggistica di Stato ---

    // Stampa messaggio informativo
    void print_info(const std::string& msg);

    // Stampa errore
    void print_error(const std::string& msg);

    // Stampa warning
    void print_warning(const std::string& msg);

    // ─── INTERVENTO #16: VERBOSE LOGGING ───────────────────────────────────────
    // Messaggio verbose: stampato solo quando -v e' attivo
    void print_verbose(const std::string& msg);

    // Flag verbose globale
    extern bool g_verbose;

} // namespace UI
