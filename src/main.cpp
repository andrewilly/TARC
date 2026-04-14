#include "ui.h"
#include "license.h"
#include "io.h"
#include "engine.h"
#include "types.h"

#include <string>
#include <vector>
#include <algorithm>
#include <cstring>

static int parse_level(const std::string& arg, int def = 3) {
    // 1. Controllo per i parametri testuali espliciti
    if (arg == "-cbest") return 9; // Imposta il livello massimo (LZMA2 Extreme)
    if (arg == "-cfast") return 1; // Imposta il livello minimo (Velocità elevata)

    // 2. Controllo per il formato numerico classico (es. -c9)
    if (arg.size() > 2 && arg.substr(0, 2) == "-c") {
        std::string ls = arg.substr(2);
        
        // Verifica che la parte rimanente della stringa sia composta solo da cifre
        if (!ls.empty() && std::all_of(ls.begin(), ls.end(), ::isdigit)) {
            try {
                // Converte in intero e limita il valore nel range supportato [1-9]
                return std::clamp(std::stoi(ls), 1, 9);
            } catch (...) {
                return def;
            }
        }
    }

    return def;
}

int main(int argc, char* argv[]) {
    UI::enable_vtp();
    UI::show_banner();
    License::check_and_activate();

    if (argc < 2) {
        UI::show_help();
        return 0;
    }

    std::string arg_cmd = argv[1];
    
    // Gestione comandi speciali estesi (-cbest, -cfast) prima del substring
    std::string cmd;
    if (arg_cmd == "-cbest" || arg_cmd == "-cfast") {
        cmd = "-c";
    } else {
        cmd = arg_cmd.substr(0, 2);
    }
    
    int level = parse_level(arg_cmd);

    if (cmd == "-h" || arg_cmd == "--help") {
        UI::show_help();
        return 0;
    }

    if (argc < 3) {
        UI::print_error("Specifica il nome dell'archivio.");
        return 1;
    }

    std::string arch = IO::ensure_ext(argv[2]);

    // COMANDO COMPRESSIONE (-c) O APPEND (-a)
    if (cmd == "-c" || cmd == "-a") {
        bool append = (cmd == "-a");
        if (argc < 4) {
            UI::print_error("Nessun file o pattern specificato.");
            return 1;
        }

        std::vector<std::string> targets;
        // Passiamo i pattern direttamente come stringhe all'Engine
        // L'Engine v2 risolverà le wildcard internamente (Windows/Linux)
        for (int i = 3; i < argc; ++i) {
            targets.push_back(argv[i]);
        }

        auto res = Engine::compress(arch, targets, append, level);
        UI::print_summary(res, append ? "Aggiunta" : "Creazione");
        return res.ok ? 0 : 1;
    }

    // COMANDO ESTRAZIONE (-x)
    if (cmd == "-x") {
        std::vector<std::string> filters;
        // Raccogliamo eventuali pattern di estrazione (es. tarc -x arch.strk *.txt)
        for (int i = 3; i < argc; ++i) {
            filters.push_back(argv[i]);
        }

        auto res = Engine::extract(arch, filters, false);
        UI::print_summary(res, "Estrazione");
        return res.ok ? 0 : 1;
    }
    
    // COMANDO TEST (-t)
    if (cmd == "-t") {
        auto res = Engine::extract(arch, {}, true);
        UI::print_summary(res, "Test integrita'");
        return res.ok ? 0 : 1;
    }

    // COMANDO LISTA (-l)
    if (cmd == "-l") {
        auto res = Engine::list(arch);
        if (!res.ok) UI::print_error("Errore lettura archivio.");
        return res.ok ? 0 : 1;
    }

    // Comando -d rimosso come richiesto (non compatibile con archivi solid v2)

    UI::print_error("Comando sconosciuto.");
    return 1;
}
