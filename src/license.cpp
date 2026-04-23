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

// Correzione Intervento #24: Inclusioni C separate dalle direttive C++
extern "C" {
#include "xxhash.h"
}

namespace fs = std::filesystem;

namespace License {

// Salt segreto per la validazione delle chiavi
const char* LICENSE_SALT = "TARC_SECURE_SALT_2024";

bool is_valid(const std::string& key) {
    if (key.size() != 32 || key.substr(0, 5) != "TARC-") return false;
    
    // Formato: TARC-XXXXXXXX-XXXXXXXX-XXXXXXXX
    // Estraiamo i blocchi
    std::string b1 = key.substr(5, 8);
    std::string b2 = key.substr(14, 8);
    std::string b3 = key.substr(23, 8);

    std::string check_str = b1 + "-" + b2 + LICENSE_SALT;
    uint32_t expected_hash = (uint32_t)(XXH64(check_str.c_str(), check_str.size(), 0) & 0xFFFFFFFF);
    
    char hex_hash[9];
    snprintf(hex_hash, 9, "%08X", expected_hash);
    
    return (b3 == hex_hash);
}

std::string get_license_path() {
    auto p = fs::temp_directory_path() / ".tarc_license";
    return p.string();
}

std::string load_saved_key() {
    std::ifstream ifs(fs::u8path(get_license_path()));
    std::string key;
    if (ifs >> key) return key;
    return "";
}

bool save_key(const std::string& key) {
    std::ofstream ofs(fs::u8path(get_license_path()));
    return (bool)(ofs << key);
}

void check_and_activate() {
    std::string key = load_saved_key();
    if (is_valid(key)) return;

    UI::print_info("Licenza non trovata o scaduta.");
    printf("Inserisci Chiave (TARC-XXXX...): ");
    std::cin >> key;

    if (is_valid(key)) {
        save_key(key);
        UI::print_success("Licenza attivata con successo!");
    } else {
        UI::print_error("Chiave non valida. Uscita.");
        std::exit(1);
    }
}

} // namespace License