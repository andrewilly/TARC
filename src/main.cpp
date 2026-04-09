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
    if (arg.size() > 2) {
        std::string ls = arg.substr(2);
        if (std::all_of(ls.begin(), ls.end(), ::isdigit))
            return std::clamp(std::stoi(ls), 1, 22);
    }
    return def;
}

int main(int argc, char* argv[]) {
    UI::enable_vtp();
    UI::show_banner(); // Mostra v1.04 definita in types.h
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

    // ─── CREA / AGGIUNGI (SMART UPDATE) ──────────────────────────────────────
    if (cmd == "-c" || cmd == "-a") {
        bool append = (cmd == "-a");
        if (argc < 4) {
            UI::print_error("Nessun file specificato.");
            return 1;
        }
        std::vector<std::string> targets;
        for (int i = 3; i < argc; ++i) IO::expand_path(argv[i], targets);

        auto res = Engine::compress(arch, targets, append, level);
        UI::print_summary(res, append ? "Aggiornamento" : "Creazione");
        return res.ok ? 0 : 1;
    }

    // ─── ESTRAI ──────────────────────────────────────────────────────────────
    if (cmd == "-x") {
        auto res = Engine::extract(arch, false);
        UI::print_summary(res, "Estrazione");
        return res.ok ? 0 : 1;
    }

    // ─── TEST ────────────────────────────────────────────────────────────────
    if (cmd == "-t") {
        auto res = Engine::extract(arch, true);
        UI::print_summary(res, "Verifica integrità");
        return res.ok ? 0 : 1;
    }

    // ─── LISTA ───────────────────────────────────────────────────────────────
    if (cmd == "-l") {
        auto res = Engine::list(arch);
        if (!res.ok) UI::print_error("Impossibile leggere l'archivio.");
        return res.ok ? 0 : 1;
    }

    UI::print_error("Comando sconosciuto.");
    return 1;
}
