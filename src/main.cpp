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
    UI::enable_vtp();
    UI::show_banner();
    License::check_and_activate();

    if (argc < 2) {
        UI::show_help();
        return 0;
    }

    bool sfx_requested = false;
    std::string arg_cmd = argv[1];
    
    // Riconoscimento parametri estesi
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

    // COMPRESSIONE O APPEND
    if (cmd == "-c" || cmd == "-a") {
        bool append = (cmd == "-a");
        std::vector<std::string> targets;
        
        for (int i = 3; i < argc; ++i) {
            std::string val = argv[i];
            if (val == "--sfx") sfx_requested = true;
            else targets.push_back(val);
        }

        if (targets.empty()) {
            UI::print_error("Nessun file specificato.");
            return 1;
        }

        auto res = Engine::compress(arch, targets, append, level);
        UI::print_summary(res, append ? "Aggiunta" : "Creazione");

        if (res.ok && sfx_requested) {
            std::string sfx_exe = arch.substr(0, arch.find_last_of('.')) + ".exe";
            auto sfx_res = Engine::create_sfx(arch, sfx_exe);
            if (sfx_res.ok) UI::print_info("Autoestraente creato: " + sfx_exe);
            else UI::print_error(sfx_res.msg);
        }
        return res.ok ? 0 : 1;
    }

    // ESTRAZIONE
    if (cmd == "-x") {
        std::vector<std::string> filters;
        for (int i = 3; i < argc; ++i) filters.push_back(argv[i]);
        auto res = Engine::extract(arch, filters, false);
        UI::print_summary(res, "Estrazione");
        return res.ok ? 0 : 1;
    }
    
    // TEST
    if (cmd == "-t") {
        auto res = Engine::extract(arch, {}, true);
        UI::print_summary(res, "Test integrita'");
        return res.ok ? 0 : 1;
    }

    // LISTA
    if (cmd == "-l") {
        auto res = Engine::list(arch);
        if (!res.ok) UI::print_error("Errore lettura archivio.");
        return res.ok ? 0 : 1;
    }

    UI::print_error("Comando sconosciuto.");
    return 1;
}
