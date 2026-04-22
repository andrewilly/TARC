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
    if (arg == "-cbest") return 9; 
    if (arg == "-cfast") return 1; 

    if (arg.size() > 2 && arg.substr(0, 2) == "-c") {
        std::string ls = arg.substr(2);
        if (!ls.empty() && std::all_of(ls.begin(), ls.end(), ::isdigit)) {
            try {
                return std::clamp(std::stoi(ls), 1, 9);
            } catch (...) {
                return def;
            }
        }
    }
    return def;
}

int main(int argc, char* argv[]) {
    // 1. Inizializzazione Ambiente e Licenza
    UI::enable_vtp();
    UI::show_banner();
    License::check_and_activate();

    if (argc < 2) {
        UI::show_help();
        return 0;
    }

    // Aggiungi dopo License::check_and_activate() nella funzione main:

    // Diagnostica per file .mdb
    UI::print_info("TARC Strike v2.01 - Modalita' diagnostica attiva");
    
    // Opzione per forzare chunk size ridotto con variabile d'ambiente
    const char* chunk_env = getenv("TARC_CHUNK_MB");
    if (chunk_env) {
        size_t mb = atoi(chunk_env);
        if (mb >= 16 && mb <= 1024) {
            Engine::set_chunk_threshold(mb * 1024 * 1024);
        }
    }
    
    // Opzione per disabilitare dedup su estensioni specifiche
    const char* skip_dedup_env = getenv("TARC_SKIP_DEDUP");
    if (skip_dedup_env) {
        std::vector<std::string> exts;
        std::string s(skip_dedup_env);
        size_t pos = 0;
        while ((pos = s.find(',')) != std::string::npos) {
            exts.push_back(s.substr(0, pos));
            s.erase(0, pos + 1);
        }
        exts.push_back(s);
        Engine::set_skip_dedup_extensions(exts);
    }
    
    bool sfx_requested = false;
    std::string arg_cmd = argv[1];
    
    // 2. Identificazione comando (gestisce -c, -a, -cbest, ecc.)
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

    // 3. LOGICA COMPRESSIONE (-c) E APPEND (-a)
    if (cmd == "-c" || cmd == "-a") {
        bool append = (cmd == "-a");
        std::vector<std::string> targets;
        
        // Rileva se tra i parametri c'è --sfx
        for (int i = 3; i < argc; ++i) {
            std::string val = argv[i];
            if (val == "--sfx") {
                sfx_requested = true;
            } else {
                targets.push_back(val);
            }
        }

        if (targets.empty()) {
            UI::print_error("Nessun file o cartella specificati.");
            return 1;
        }

        // Avvio motore di compressione (Chunk 256MB gestiti in engine.cpp)
        auto res = Engine::compress(arch, targets, append, level);
        UI::print_summary(res, append ? "Aggiunta" : "Creazione");

        // Se l'operazione è riuscita e l'utente ha chiesto SFX
        if (res.ok && sfx_requested) {
            std::string sfx_exe = arch.substr(0, arch.find_last_of('.')) + ".exe";
            auto sfx_res = Engine::create_sfx(arch, sfx_exe);
            if (sfx_res.ok) {
                UI::print_info("Autoestraente generato correttamente: " + sfx_exe);
            } else {
                UI::print_error(sfx_res.message);
            }
        }
        return res.ok ? 0 : 1;
    }

    // 4. LOGICA ESTRAZIONE (-x)
    if (cmd == "-x") {
        std::vector<std::string> filters;
        bool flat_mode = false;

        // Parsing argomenti opzionali
        for (int i = 3; i < argc; ++i) {
            std::string val = argv[i];
            if (val == "--flat") {
                flat_mode = true;
            } else {
                filters.push_back(val);
            }
        }
        
        // Feedback modalità
        if (flat_mode) UI::print_info("Modalita' Flat attiva: percorsi ignorati.");
        if (!filters.empty()) UI::print_info("Filtri attivi: " + std::to_string(filters.size()));
        
        auto res = Engine::extract(arch, filters, false, 0, flat_mode);
        UI::print_summary(res, "Estrazione");
        return res.ok ? 0 : 1;
    }
    
    // 5. LOGICA TEST (-t)
    if (cmd == "-t") {
        auto res = Engine::extract(arch, {}, true);
        UI::print_summary(res, "Test integrita'");
        return res.ok ? 0 : 1;
    }

    // 6. LOGICA LISTA (-l)
    if (cmd == "-l") {
        auto res = Engine::list(arch);
        if (!res.ok) UI::print_error("Errore lettura archivio: " + res.message);
        return res.ok ? 0 : 1;
    }

    UI::print_error("Comando sconosciuto.");
    return 1;
}
