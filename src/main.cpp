#include "tarc/security/license.h"
#include "tarc/core/compressor.h"
#include "tarc/ui/ui.h"
#include <iostream>
#include <vector>
#include <string>
#include <algorithm>

using namespace tarc;

// ═══════════════════════════════════════════════════════════════════════════
// ARGUMENT PARSING
// ═══════════════════════════════════════════════════════════════════════════

struct CommandArgs {
    std::string command;
    std::string archive_path;
    std::vector<std::string> targets;
    int compression_level = 3;
    bool flat_mode = false;
    bool create_sfx = false;
};

static int parse_compression_level(const std::string& arg) {
    if (arg == "-cbest") return 9;
    if (arg == "-cfast") return 1;
    
    if (arg.size() > 2 && arg.substr(0, 2) == "-c") {
        std::string level_str = arg.substr(2);
        if (!level_str.empty() && std::all_of(level_str.begin(), level_str.end(), ::isdigit)) {
            try {
                return std::clamp(std::stoi(level_str), 1, 9);
            } catch (...) {}
        }
    }
    
    return 3;  // default
}

static CommandArgs parse_arguments(int argc, char* argv[]) {
    CommandArgs args;
    
    if (argc < 2) return args;
    
    std::string first_arg = argv[1];
    
    // Gestione -cbest/-cfast
    if (first_arg == "-cbest" || first_arg == "-cfast") {
        args.command = "-c";
        args.compression_level = parse_compression_level(first_arg);
    } else {
        args.command = first_arg.substr(0, 2);
        args.compression_level = parse_compression_level(first_arg);
    }
    
    // Archivio (secondo parametro)
    if (argc >= 3) {
        args.archive_path = argv[2];
        if (args.archive_path.find(".strk") == std::string::npos) {
            args.archive_path += ".strk";
        }
    }
    
    // Parametri aggiuntivi
    for (int i = 3; i < argc; ++i) {
        std::string param = argv[i];
        
        if (param == "--sfx") {
            args.create_sfx = true;
        } else if (param == "--flat") {
            args.flat_mode = true;
        } else {
            args.targets.push_back(param);
        }
    }
    
    return args;
}

// ═══════════════════════════════════════════════════════════════════════════
// MAIN
// ═══════════════════════════════════════════════════════════════════════════

int main(int argc, char* argv[]) {
    // Inizializzazione
    ui::enable_vtp();
    ui::show_banner();
    security::check_and_activate();
    
    // Help
    if (argc < 2 || std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help") {
        ui::show_help();
        return 0;
    }
    
    // Parse args
    auto args = parse_arguments(argc, argv);
    
    if (args.archive_path.empty()) {
        ui::print_error("Specifica il nome dell'archivio.");
        return 1;
    }
    
    // ═══════════════════════════════════════════════════════════════════════
    // COMPRESS / APPEND
    // ═══════════════════════════════════════════════════════════════════════
    
    if (args.command == "-c" || args.command == "-a") {
        if (args.targets.empty()) {
            ui::print_error("Nessun file specificato.");
            return 1;
        }
        
        core::Compressor compressor;
        core::Compressor::Options opts;
        opts.compression_level = args.compression_level;
        opts.append_mode = (args.command == "-a");
        opts.create_sfx = args.create_sfx;
        
        auto result = compressor.compress(args.archive_path, args.targets, opts);
        
        ui::print_summary(result, opts.append_mode ? "Append" : "Compress");
        return result.ok() ? 0 : 1;
    }
    
    // ═══════════════════════════════════════════════════════════════════════
    // EXTRACT (placeholder - da implementare in extractor.cpp)
    // ═══════════════════════════════════════════════════════════════════════
    
    if (args.command == "-x") {
        ui::print_error("Estrazione non ancora implementata in questa versione.");
        return 1;
    }
    
    // ═══════════════════════════════════════════════════════════════════════
    // LIST / TEST (placeholder)
    // ═══════════════════════════════════════════════════════════════════════
    
    if (args.command == "-l" || args.command == "-t") {
        ui::print_error("Comando non ancora implementato.");
        return 1;
    }
    
    ui::print_error("Comando sconosciuto: " + args.command);
    return 1;
}
