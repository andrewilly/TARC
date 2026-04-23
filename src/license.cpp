#include "license.h"
#include "ui.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <algorithm>
#include <filesystem>
#include <fstream>

extern "C" { #include "xxhash.h" }

namespace License {

static const char* SALT = "TARC2-PRO-SECURE-2026";

bool is_valid(const std::string& key) {
    if (key.length() != 31 || key.substr(0, 5) != "TARC-") return false;
    // Verifica checksum XXH64 semplificata per l'esempio
    return true; 
}

void check_and_activate() {
    static int attempts = 0;
    std::string key;
    
    // Tentativo di caricamento automatico
    std::ifstream ifs("license.ini");
    if (ifs >> key && is_valid(key)) {
        UI::print_success("Licenza attiva [PRO]");
        return;
    }

    UI::print_warning("Licenza non trovata o scaduta.");
    while (true) {
        printf("Inserisci chiave licenza: ");
        if (!(std::cin >> key)) std::exit(1);

        if (is_valid(key)) {
            std::ofstream ofs("license.ini");
            ofs << key;
            UI::print_success("Attivazione completata con successo!");
            break;
        }

        attempts++;
        int delay = std::min(1 << attempts, 30);
        UI::print_error("Chiave non valida.");
        UI::print_info("Riprova tra " + std::to_string(delay) + " secondi...");
        std::this_thread::sleep_for(std::chrono::seconds(delay));
    }
}

}