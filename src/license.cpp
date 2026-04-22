#include "license.h"
#include "ui.h"
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

namespace License {

bool is_valid(const std::string& key) {
    if (key.size() < 12 || key.substr(0, 5) != "TARC-") return false;
    int sum = 0;
    for (char c : key) sum += static_cast<int>(c);
    return sum % 7 == 0;
}

std::string get_license_path() {
#ifdef _WIN32
    const char* appdata = getenv("APPDATA");
    return appdata ? std::string(appdata) + "\\TARC\\license.ini" : "license.ini";
#else
    const char* home = getenv("HOME");
    return home ? std::string(home) + "/.tarc_license.ini" : ".tarc_license.ini";
#endif
}

std::string load_saved_key() {
    std::string key;
    std::ifstream ifs(get_license_path());
    if (ifs) ifs >> key;
    return key;
}

bool save_key(const std::string& key) {
    try {
        fs::path p(get_license_path());
        fs::create_directories(p.parent_path());
        std::ofstream ofs(p);
        if (!ofs) return false;
        ofs << key;
        return true;
    } catch (...) {
        return false;
    }
}

void check_and_activate() {
    std::string key = load_saved_key();

    if (is_valid(key)) {
        printf("%s[LICENSE]%s Attiva  (%s%s%s)\n",
               Color::CYAN, Color::RESET,
               Color::GREEN, key.c_str(), Color::RESET);
        return;
    }

    printf("\n%s%s╔══════════════════════════════╗\n"
           "║    TARC LICENSE MANAGER      ║\n"
           "╚══════════════════════════════╝%s\n",
           Color::BOLD, Color::CYAN, Color::RESET);

    printf(" %sLicenza non trovata o non valida.%s\n Inserisci chiave: ",
           Color::RED, Color::RESET);

    std::cin >> key;
    
    if (!is_valid(key)) {
        UI::print_error("Chiave non valida. Operazione annullata.");
        std::exit(1);
    }

    if (!save_key(key)) {
        UI::print_warning("Impossibile salvare la licenza su disco.");
    }

    printf("%s✔ Attivazione riuscita!%s\n\n", Color::GREEN, Color::RESET);
}

} // namespace License
