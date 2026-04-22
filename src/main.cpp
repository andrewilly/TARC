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

static int parse_level(const std::string& arg, int def = 6) {
    if (arg == "-cbest") return 9;
    if (arg == "-cfast") return 1;
    
    if (arg.size() > 2 && arg.substr(0, 2) == "-c") {
        std::string ls = arg.substr(2);
        if (!ls.empty() && std::all_of(ls.begin(), ls.end(), ::isdigit)) {
            try {
                int val = std::stoi(ls);
                return std::clamp(val, 1, 9);
            } catch (...) {
                return def;
            }
        }
    }
    return def;
}

static void load_environment_config() {
    const char* chunk_env = std::getenv("TARC_CHUNK_MB");
    if (chunk_env) {
        size_t mb = static_cast<size_t>(std::atoi(chunk_env));
        if (mb >= 16 && mb <= 1024) {
            Engine::set_chunk_threshold(mb * 1024 * 1024);
        }
    }
    
    const char* workers_env = std::getenv("TARC_WORKERS");
    if (workers_env) {
        size_t workers = static_cast<size_t>(std::atoi(workers_env));
        if (workers >= 1 && workers <= 64) {
            Engine::set_compression_workers(workers);
        }
    }
    
    const char* skip_dedup_env = std::getenv("TARC_SKIP_DEDUP");
    if (skip_dedup_env) {
        std::vector<std::string> exts;
        std::string s(skip_dedup_env);
        size_t pos = 0;
        while ((pos = s.find(',')) != std::string::npos) {
            exts.push_back(s.substr(0, pos));
            s.erase(0, pos + 1);
        }
        exts.push_back(s);
        CodecSelector::set_skip_extensions(exts);
        UI::print_info("Deduplicazione disabilitata per: " + s);
    }
}

int main(int argc, char* argv[]) {
    UI::enable_vtp();
    UI::show_banner();
    License::check_and_activate();
    load_environment_config();

    if (argc < 2) {
        UI::show_help();
        return 0;
    }

    bool sfx_requested = false;
    std::string arg_cmd = argv[1];
    
    std::string cmd;
    if (arg_cmd == "-cbest" || arg_cmd == "-cfast") {
        cmd = "-c";
    } else if (arg_cmd.size() >= 2) {
        cmd = arg_cmd.substr(0, 2);
    } else {
        cmd = arg_cmd;
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

    // Compressione
    if (cmd == "-c" || cmd == "-a") {
        bool append = (cmd == "-a");
        std::vector<std::string> targets;
        
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

        auto res = Engine::compress(arch, targets, append, level);
        UI::print_summary(res, append ? "Aggiunta" : "Creazione");

        if (res.ok && sfx_requested) {
            std::string sfx_exe = arch.substr(0, arch.find_last_of('.')) + ".exe";
            auto sfx_res = Engine::create_sfx(arch, sfx_exe);
            if (sfx_res.ok) {
                UI::print_info("Autoestraente generato: " + sfx_exe);
            } else {
                UI::print_error(sfx_res.message);
            }
        }
        return res.ok ? 0 : 1;
    }

    // Estrazione
    if (cmd == "-x") {
        std::vector<std::string> filters;
        bool flat_mode = false;

        for (int i = 3; i < argc; ++i) {
            std::string val = argv[i];
            if (val == "--flat") {
                flat_mode = true;
            } else {
                filters.push_back(val);
            }
        }
        
        if (flat_mode) UI::print_info("Modalità Flat attiva");
        if (!filters.empty()) UI::print_info("Filtri: " + std::to_string(filters.size()));
        
        auto res = Engine::extract(arch, filters, false, 0, flat_mode);
        UI::print_summary(res, "Estrazione");
        return res.ok ? 0 : 1;
    }
    
    // Test
    if (cmd == "-t") {
        auto res = Engine::extract(arch, {}, true);
        UI::print_summary(res, "Test integrità");
        return res.ok ? 0 : 1;
    }

    // Lista
    if (cmd == "-l") {
        auto res = Engine::list(arch);
        if (!res.ok) UI::print_error("Errore: " + res.message);
        return res.ok ? 0 : 1;
    }

    UI::print_error("Comando sconosciuto: " + cmd);
    return 1;
}
