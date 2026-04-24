#include "license.h"
#include "ui.h"
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <thread>
#include <chrono>
#include <cstring>
#include <algorithm>

// XXH64 per validazione licenza
extern "C" {
    #include "xxhash.h"
}

namespace fs = std::filesystem;

namespace License {

// ═══════════════════════════════════════════════════════════════════════════════
// VALIDAZIONE LICENZA
//
// Formato chiave: TARC-XXXXXXXX-XXXXXXXX-XXXXXXXX
//   Gruppo 1 (8 hex): Identificativo utente/prodotto
//   Gruppo 2 (8 hex): Feature flags / scadenza / estensione
//   Gruppo 3 (8 hex): Checksum = XXH64(gruppo1 + "-" + gruppo2 + SALT) & 0xFFFFFFFF
//
// IMPORTANTE: Il SALT seguente va modificato UNA SOLA VOLTA prima della prima
// release pubblica, e MAI PIU' cambiato. Ogni modifica rende invalidate tutte
// le chiavi precedentemente emesse. In un ambiente enterprise, derivare il
// salt da una chiave master tramite KDF.
// ═══════════════════════════════════════════════════════════════════════════════

static const char* const LICENSE_SALT = "TARC2-S4LT-2026-STRIKE-VLD";

// ─── HELPER: Verifica se un carattere e' hex ──────────────────────────────────
static bool is_hex_char(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// ─── HELPER: Calcola il checksum di un payload licenza ────────────────────────
static uint32_t compute_license_checksum(const std::string& group1, const std::string& group2) {
    std::string payload = group1 + "-" + group2 + LICENSE_SALT;
    XXH64_hash_t hash = XXH64(payload.c_str(), payload.size(), 0);
    return static_cast<uint32_t>(hash & 0xFFFFFFFF);
}

// ─── HELPER: Confronto a tempo costante migliorato ────────────────────────────
// Accumula le differenze bit-a-bit invece di usare XOR + confronto diretto,
// per resistere meglio ad attacchi timing su architetture con short-circuit.
static bool constant_time_compare(uint32_t a, uint32_t b) {
    uint32_t diff = a ^ b;
    // Accumula tutti i bit di differenza: se diff e' 0, tutti i bit sono 0
    // e il risultato sara' 0. Altrimenti sara' != 0.
    diff |= diff >> 16;
    diff |= diff >> 8;
    diff |= diff >> 4;
    diff |= diff >> 2;
    diff |= diff >> 1;
    return (diff ^ 1) & 1;
}

// ─── VALIDAZIONE ─────────────────────────────────────────────────────────────
bool is_valid(const std::string& key) {
    // Formato atteso: TARC-XXXXXXXX-XXXXXXXX-XXXXXXXX (totale 31 caratteri)
    if (key.length() != 31) return false;

    // Verifica prefisso
    if (key.substr(0, 5) != "TARC-") return false;

    // Verifica posizioni dei trattini
    if (key[13] != '-' || key[22] != '-') return false;

    // Estrai i 3 gruppi (8 hex ciascuno)
    std::string group1 = key.substr(5, 8);   // pos 5-12
    std::string group2 = key.substr(14, 8);  // pos 14-21
    std::string group3 = key.substr(23, 8);  // pos 23-30

    // Verifica che tutti i caratteri siano hex
    for (char c : group1) if (!is_hex_char(c)) return false;
    for (char c : group2) if (!is_hex_char(c)) return false;
    for (char c : group3) if (!is_hex_char(c)) return false;

    // Normalizza a lowercase per il calcolo
    auto to_lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
    std::string g1 = group1, g2 = group2, g3 = group3;
    std::transform(g1.begin(), g1.end(), g1.begin(), to_lower);
    std::transform(g2.begin(), g2.end(), g2.begin(), to_lower);
    std::transform(g3.begin(), g3.end(), g3.begin(), to_lower);

    // Calcola checksum atteso
    uint32_t expected = compute_license_checksum(g1, g2);

    // Converte group3 da hex a uint32
    uint32_t provided = 0;
    try {
        provided = static_cast<uint32_t>(std::stoul(g3, nullptr, 16));
    } catch (...) {
        return false;
    }

    // Confronto a tempo costante
    return constant_time_compare(expected, provided);
}

// ─── PERCORSO FILE (Unicode-safe) ────────────────────────────────────────────
std::string get_license_path() {
#ifdef _WIN32
    const wchar_t* appdata = _wgetenv(L"APPDATA");
    if (appdata) {
        fs::path p = fs::path(appdata) / L"TARC" / L"license.ini";
        return p.u8string();
    }
    return "license.ini";
#else
    const char* home = getenv("HOME");
    return home
        ? std::string(home) + "/.tarc_license.ini"
        : ".tarc_license.ini";
#endif
}

// ─── CARICA / SALVA (Unicode-safe) ───────────────────────────────────────────
std::string load_saved_key() {
    std::string path = get_license_path();
    std::string key;

#ifdef _WIN32
    std::ifstream ifs(fs::u8path(path));
#else
    std::ifstream ifs(path);
#endif
    if (ifs) ifs >> key;
    return key;
}

bool save_key(const std::string& key) {
    try {
        std::string path = get_license_path();
#ifdef _WIN32
        fs::path p = fs::u8path(path);
#else
        fs::path p = path;
#endif
        fs::create_directories(p.parent_path());

#ifdef _WIN32
        std::ofstream ofs(p);
#else
        std::ofstream ofs(path);
#endif
        if (!ofs) return false;
        ofs << key;
        return true;
    } catch (...) {
        return false;
    }
}

// ─── CHECK & ACTIVATE (con brute-force protection) ───────────────────────────
void check_and_activate() {
    // Brute-force protection: delay raddoppia ad ogni tentativo, fino a 60 sec
    static int failed_attempts = 0;

    auto apply_brute_force_delay = [&]() {
        failed_attempts++;
        int delay_sec = std::min(1 << std::min(failed_attempts, 6), 60);
        if (delay_sec > 1) {
            UI::print_warning("Troppi tentativi falliti. Attesa " +
                              std::to_string(delay_sec) + " secondi...");
        }
        std::this_thread::sleep_for(std::chrono::seconds(delay_sec));
    };

    // ── Prova a caricare la chiave salvata ──────────────────────────────────
    std::string key = load_saved_key();

    if (is_valid(key)) {
        printf("%s[LICENSE]%s Attiva  (%s%s%s)\n",
               UI::g_color_enabled ? Color::CYAN : "",
               UI::g_color_enabled ? Color::RESET : "",
               UI::g_color_enabled ? Color::GREEN : "",
               key.c_str(),
               UI::g_color_enabled ? Color::RESET : "");
        failed_attempts = 0;
        return;
    }

    // ── Chiave non trovata o non valida: chiedi all'utente ──────────────────
    const char* C = UI::g_color_enabled ? Color::CYAN : "";
    const char* R = UI::g_color_enabled ? Color::RESET : "";
    const char* B = UI::g_color_enabled ? Color::BOLD : "";
    const char* G = UI::g_color_enabled ? Color::GREEN : "";
    const char* R2 = UI::g_color_enabled ? Color::RED : "";

    printf("\n%s%s+================================+\n"
           "|    TARC LICENSE MANAGER v2     |\n"
           "+================================+%s\n",
           B, C, R);

    printf(" %sLicenza non trovata o non valida.%s\n", R2, R);
    printf(" Formato: TARC-XXXXXXXX-XXXXXXXX-XXXXXXXX\n");
    printf(" Inserisci chiave: ");

    if (!(std::cin >> key)) {
        UI::print_error("Input non valido. Operazione annullata.");
        std::exit(1);
    }

    if (!is_valid(key)) {
        apply_brute_force_delay();

        // Seconda opportunita'
        printf(" %sChiave non valida.%s Riprova: ", R2, R);
        if (!(std::cin >> key) || !is_valid(key)) {
            apply_brute_force_delay();
            UI::print_error("Chiave non valida. Operazione annullata.");
            std::exit(1);
        }
    }

    if (!save_key(key)) {
        UI::print_warning("Impossibile salvare la licenza su disco.");
    }

    failed_attempts = 0;
    printf("%s+ Attivazione riuscita!%s\n\n", G, R);
}

} // namespace License
