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

    // ═══════════════════════════════════════════════════════════════════════════
    // LOGICA COMPRESSIONE (-c) E APPEND (-a)
    // Intervento #15: --exclude patterns
    // Intervento #16: -v verbose
    // ═══════════════════════════════════════════════════════════════════════════
    if (cmd == "-c" || cmd == "-a") {
        bool append = (cmd == "-a");
        std::vector<std::string> targets;
        std::vector<std::string> exclude_patterns;

        for (int i = 3; i < argc; ++i) {
            std::string val = argv[i];
            if (val == "--sfx") {
                sfx_requested = true;
            } else if (val == "-v") {
                UI::g_verbose = true;
                UI::print_verbose("Modalita' verbose attiva.");
            } else if (val == "--exclude" && i + 1 < argc) {
                // Il pattern seguente e' l'argomento di --exclude
                ++i;
                exclude_patterns.push_back(argv[i]);
                UI::print_verbose("Exclude pattern: " + std::string(argv[i]));
            } else {
                targets.push_back(val);
            }
        }

        if (targets.empty()) {
            UI::print_error("Nessun file o cartella specificati.");
            return 1;
        }

        // Avvio motore di compressione
        UI::progress_timer_reset();
        auto res = Engine::compress(arch, targets, append, level, exclude_patterns);
        UI::print_summary(res, append ? "Aggiunta" : "Creazione");

        // Se l'operazione e' riuscita e l'utente ha chiesto SFX
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

    // ═══════════════════════════════════════════════════════════════════════════
    // LOGICA ESTRAZIONE (-x)
    // Intervento #14: -o output directory
    // Intervento #16: -v verbose
    // ═══════════════════════════════════════════════════════════════════════════
    if (cmd == "-x") {
        std::vector<std::string> filters;
        bool flat_mode = false;
        std::string output_dir;

        // Parsing argomenti opzionali
        for (int i = 3; i < argc; ++i) {
            std::string val = argv[i];
            if (val == "--flat") {
                flat_mode = true;
            } else if (val == "-v") {
                UI::g_verbose = true;
                UI::print_verbose("Modalita' verbose attiva.");
            } else if (val == "-o" && i + 1 < argc) {
                // La directory seguente e' l'argomento di -o
                ++i;
                output_dir = argv[i];
            } else {
                filters.push_back(val);
            }
        }

        // Feedback modalita'
        if (flat_mode) UI::print_info("Modalita' Flat attiva: percorsi ignorati.");
        if (!output_dir.empty()) UI::print_info("Directory di output: " + output_dir);
        if (!filters.empty()) UI::print_info("Filtri attivi: " + std::to_string(filters.size()));

        UI::progress_timer_reset();
        auto res = Engine::extract(arch, filters, false, 0, flat_mode, output_dir);
        UI::print_summary(res, "Estrazione");
        return res.ok ? 0 : 1;
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // LOGICA TEST (-t)
    // ═══════════════════════════════════════════════════════════════════════════
    if (cmd == "-t") {
        for (int i = 3; i < argc; ++i) {
            if (std::string(argv[i]) == "-v") {
                UI::g_verbose = true;
                UI::print_verbose("Modalita' verbose attiva.");
            }
        }

        UI::progress_timer_reset();
        auto res = Engine::extract(arch, {}, true);
        UI::print_summary(res, "Test integrita'");
        return res.ok ? 0 : 1;
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // LOGICA LISTA (-l)
    // ═══════════════════════════════════════════════════════════════════════════
    if (cmd == "-l") {
        auto res = Engine::list(arch);
        if (!res.ok) UI::print_error("Errore lettura archivio: " + res.message);
        else UI::print_summary(res, "Lista");
        return res.ok ? 0 : 1;
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // LOGICA RIMOZIONE (-r) — Intervento #21
    // ═══════════════════════════════════════════════════════════════════════════
    if (cmd == "-r") {
        std::vector<std::string> patterns;
        for (int i = 3; i < argc; ++i) {
            std::string val = argv[i];
            if (val == "-v") {
                UI::g_verbose = true;
                UI::print_verbose("Modalita' verbose attiva.");
            } else {
                patterns.push_back(val);
            }
        }

        if (patterns.empty()) {
            UI::print_error("Specifica almeno un pattern per la rimozione.");
            return 1;
        }

        UI::print_info("Rimozione file dall'archivio (riscrittura completa)...");
        UI::progress_timer_reset();
        auto res = Engine::remove_files(arch, patterns);
        UI::print_summary(res, "Rimozione");
        return res.ok ? 0 : 1;
    }

    UI::print_error("Comando sconosciuto.");
    return 1;
}
