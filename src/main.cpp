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
    if (arg == "-cbest") return 22; // Imposta il livello massimo (ZSTD 22 / LZMA Ultra)
    if (arg == "-cfast") return 1;  // Imposta il livello minimo (LZ4 / ZSTD 1)

    // 2. Controllo per il formato numerico classico (es. -c9)
    // Verifichiamo che la stringa inizi con "-c" e abbia dei numeri a seguire
    if (arg.size() > 2 && arg.substr(0, 2) == "-c") {
        std::string ls = arg.substr(2);
        
        // Verifica che la parte rimanente della stringa sia composta solo da cifre
        if (!ls.empty() && std::all_of(ls.begin(), ls.end(), ::isdigit)) {
            try {
                // Converte in intero e limita il valore nel range supportato [1-22]
                return std::clamp(std::stoi(ls), 1, 22);
            } catch (...) {
                // In caso di errore di conversione (es. overflow), restituisce il default
                return def;
            }
        }
    }

    // Restituisce il valore di default se l'argomento non è riconosciuto
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
    std::string cmd     = arg_cmd.substr(0, 2);
    int         level   = parse_level(arg_cmd);

    if (cmd == "-h" || cmd == "--") {
        UI::show_help();
        return 0;
    }

    if (argc < 3) {
        UI::print_error("Specifica il nome dell'archivio.");
        return 1;
    }

    std::string arch = IO::ensure_ext(argv[2]);

    if (cmd == "-c" || cmd == "-a") {
        bool append = (cmd == "-a");
        if (argc < 4) {
            UI::print_error("Nessun file specificato.");
            return 1;
        }
        std::vector<std::string> targets;
        for (int i = 3; i < argc; ++i) IO::expand_path(argv[i], targets);

        auto res = Engine::compress(arch, targets, append, level);
        UI::print_summary(res, append ? "Aggiunta" : "Creazione");
        return res.ok ? 0 : 1;
    }

    if (cmd == "-x") {
        auto res = Engine::extract(arch, false);
        UI::print_summary(res, "Estrazione");
        return res.ok ? 0 : 1;
    }

    if (cmd == "-t") {
        auto res = Engine::extract(arch, true);
        UI::print_summary(res, "Test integrità");
        return res.ok ? 0 : 1;
    }

    if (cmd == "-l") {
        auto res = Engine::list(arch);
        if (!res.ok) UI::print_error("Errore lettura archivio.");
        return res.ok ? 0 : 1;
    }

    if (cmd == "-d") {
        std::vector<std::string> targets;
        for (int i = 3; i < argc; ++i) IO::expand_path(argv[i], targets);
        auto res = Engine::remove_files(arch, targets);
        UI::print_summary(res, "Eliminazione");
        return res.ok ? 0 : 1;
    }

    UI::print_error("Comando sconosciuto.");
    return 1;
}
