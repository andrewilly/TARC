#include "ui.h"
#include "license.h"
#include "io.h"
#include "engine.h"
#include "types.h"

#include <string>
#include <vector>
#include <algorithm>
#include <cstring>
#include <cstdlib>

// ═══════════════════════════════════════════════════════════════════════════════
// ARGOMENTI DA RIGA DI COMANDO — Struct centralizzata
// ═══════════════════════════════════════════════════════════════════════════════

enum class Command { Compress, Append, Extract, Test, List, Remove, Help };

struct CliArgs {
    Command cmd = Command::Help;
    std::string archive;
    int level = 3;
    bool sfx = false;
    bool flat_mode = false;
    std::string output_dir;
    std::vector<std::string> targets;          // file da comprimere
    std::vector<std::string> filters;          // filtri per extract/remove
    std::vector<std::string> exclude_patterns; // pattern di esclusione
};

// ─── PARSING LIVELLO COMPRESSIONE ────────────────────────────────────────────
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

// ─── PARSING ARGOMENTI ───────────────────────────────────────────────────────
static CliArgs parse_args(int argc, char* argv[]) {
    CliArgs args;

    if (argc < 2) {
        args.cmd = Command::Help;
        return args;
    }

    std::string arg_cmd = argv[1];

    // Identificazione comando
    if (arg_cmd == "-h" || arg_cmd == "--help") {
        args.cmd = Command::Help;
        return args;
    }

    args.level = parse_level(arg_cmd);

    // Determina il comando base
    std::string cmd;
    if (arg_cmd == "-cbest" || arg_cmd == "-cfast") {
        cmd = "-c";
    } else {
        cmd = arg_cmd.substr(0, 2);
    }

    if (cmd == "-c")       args.cmd = Command::Compress;
    else if (cmd == "-a")  args.cmd = Command::Append;
    else if (cmd == "-x")  args.cmd = Command::Extract;
    else if (cmd == "-t")  args.cmd = Command::Test;
    else if (cmd == "-l")  args.cmd = Command::List;
    else if (cmd == "-r")  args.cmd = Command::Remove;
    else {
        args.cmd = Command::Help;
        return args;
    }

    if (argc < 3) return args;

    args.archive = IO::ensure_ext(argv[2]);

    // Parsing opzioni e argomenti rimanenti (unificato per tutti i comandi)
    for (int i = 3; i < argc; ++i) {
        std::string val = argv[i];

        if (val == "--sfx") {
            args.sfx = true;
        } else if (val == "-v") {
            UI::g_verbose = true;
        } else if (val == "--flat") {
            args.flat_mode = true;
        } else if (val == "--exclude" && i + 1 < argc) {
            ++i;
            args.exclude_patterns.push_back(argv[i]);
            UI::print_verbose("Exclude pattern: " + std::string(argv[i]));
        } else if (val == "-o" && i + 1 < argc) {
            ++i;
            args.output_dir = argv[i];
        } else {
            // Argomento posizionale: target (compress) o filtro (extract/remove)
            args.targets.push_back(val);
            args.filters.push_back(val);
        }
    }

    if (UI::g_verbose) {
        UI::print_verbose("Modalita' verbose attiva.");
    }

    return args;
}

// ═══════════════════════════════════════════════════════════════════════════════
// MAIN
// ═══════════════════════════════════════════════════════════════════════════════

int main(int argc, char* argv[]) {
    // 1. Inizializzazione Ambiente e Licenza
    UI::enable_vtp();
    UI::show_banner();
    License::check_and_activate();

    // 2. Parsing argomenti
    CliArgs args = parse_args(argc, argv);

    if (args.cmd == Command::Help) {
        UI::show_help();
        return 0;
    }

    if (args.archive.empty()) {
        UI::print_error("Specifica il nome dell'archivio.");
        return 1;
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // COMPRESSIONE (-c) E APPEND (-a)
    // ═══════════════════════════════════════════════════════════════════════════
    if (args.cmd == Command::Compress || args.cmd == Command::Append) {
        bool append = (args.cmd == Command::Append);

        if (args.targets.empty()) {
            UI::print_error("Nessun file o cartella specificati.");
            return 1;
        }

        UI::progress_timer_reset();
        auto res = Engine::compress(args.archive, args.targets, append, args.level, args.exclude_patterns);
        UI::print_summary(res, append ? "Aggiunta" : "Creazione");

        // Generazione SFX se richiesto e operazione riuscita
        if (res.ok && args.sfx) {
            std::string sfx_exe = args.archive.substr(0, args.archive.find_last_of('.')) + ".exe";
            auto sfx_res = Engine::create_sfx(args.archive, sfx_exe);
            if (sfx_res.ok) {
                UI::print_info("Autoestraente generato correttamente: " + sfx_exe);
            } else {
                UI::print_error(sfx_res.message);
            }
        }
        return res.ok ? 0 : 1;
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // ESTRAZIONE (-x)
    // ═══════════════════════════════════════════════════════════════════════════
    if (args.cmd == Command::Extract) {
        if (args.flat_mode) UI::print_info("Modalita' Flat attiva: percorsi ignorati.");
        if (!args.output_dir.empty()) UI::print_info("Directory di output: " + args.output_dir);
        if (!args.filters.empty()) UI::print_info("Filtri attivi: " + std::to_string(args.filters.size()));

        UI::progress_timer_reset();
        auto res = Engine::extract(args.archive, args.filters, false, 0, args.flat_mode, args.output_dir);
        UI::print_summary(res, "Estrazione");
        return res.ok ? 0 : 1;
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // TEST (-t)
    // ═══════════════════════════════════════════════════════════════════════════
    if (args.cmd == Command::Test) {
        UI::progress_timer_reset();
        auto res = Engine::extract(args.archive, {}, true);
        UI::print_summary(res, "Test integrita'");
        return res.ok ? 0 : 1;
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // LISTA (-l)
    // ═══════════════════════════════════════════════════════════════════════════
    if (args.cmd == Command::List) {
        auto res = Engine::list(args.archive);
        if (!res.ok) UI::print_error("Errore lettura archivio: " + res.message);
        else UI::print_summary(res, "Lista");
        return res.ok ? 0 : 1;
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // RIMOZIONE (-r)
    // ═══════════════════════════════════════════════════════════════════════════
    if (args.cmd == Command::Remove) {
        if (args.filters.empty()) {
            UI::print_error("Specifica almeno un pattern per la rimozione.");
            return 1;
        }

        UI::print_info("Rimozione file dall'archivio (riscrittura completa)...");
        UI::progress_timer_reset();
        auto res = Engine::remove_files(args.archive, args.filters);
        UI::print_summary(res, "Rimozione");
        return res.ok ? 0 : 1;
    }

    UI::print_error("Comando sconosciuto.");
    return 1;
}
