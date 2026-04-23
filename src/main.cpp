#include "ui.h"
#include "license.h"
#include "engine.h"
#include "io.h"
#include <iostream>

int main(int argc, char* argv[]) {
    UI::enable_vtp();
    UI::show_banner();
    License::check_and_activate();

    if (argc < 3) {
        UI::show_help();
        return 0;
    }

    std::string cmd = argv[1];
    std::string arch = IO::ensure_ext(argv[2]);
    TarcResult res;

    if (cmd == "-c" || cmd == "-a") {
        std::vector<std::string> targets;
        for (int i = 3; i < argc; ++i) targets.push_back(argv[i]);
        res = Engine::compress(arch, targets, (cmd == "-a"), 3);
        UI::print_summary(res, "Compressione");
    } 
    else if (cmd == "-x") {
        res = Engine::extract(arch, {});
        UI::print_summary(res, "Estrazione");
    }
    else if (cmd == "-l") {
        res = Engine::list(arch);
        UI::print_summary(res, "Contenuto Archivio");
    }
    else {
        UI::print_error("Comando non riconosciuto.");
        return 1;
    }

    return res.ok ? 0 : 1;
}