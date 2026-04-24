#include "ui.h"
#include "license.h"
#include "io.h"
#include "engine.h"
#include "types.h"

#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <cstring>
#include <chrono>
#include <filesystem>

namespace fs = std::filesystem;

struct Command {
    enum Type {
        None,
        Create,
        Add,
        Extract,
        List,
        Test,
        Delete,
        Help,
        License,
        Version
    } type = None;
    
    int level = 3;
    bool append = false;
    bool sfx = false;
    bool flat = false;
    bool force = false;
    bool test_only = false;
    std::string archive;
    std::vector<std::string> files;
    std::vector<std::string> filters;
};

static Command parse_args(int argc, char* argv[]) {
    Command cmd;
    
    if (argc < 2) {
        cmd.type = Command::Help;
        return cmd;
    }
    
    std::string arg = argv[1];
    
    if (arg == "--help" || arg == "-h") {
        cmd.type = Command::Help;
        return cmd;
    }
    
    if (arg == "--version" || arg == "-v") {
        cmd.type = Command::Version;
        return cmd;
    }
    
    if (arg == "--license") {
        cmd.type = Command::License;
        return cmd;
    }
    
    std::string prefix = arg.substr(0, 2);
    
    if (prefix == "-c") {
        cmd.type = Command::Create;
        cmd.level = 3;
        
        if (arg == "-cbest") {
            cmd.level = 9;
        } else if (arg == "-cfast") {
            cmd.level = 1;
        } else if (arg.length() > 2) {
            std::string level_str = arg.substr(2);
            if (!level_str.empty() && std::all_of(level_str.begin(), level_str.end(), ::isdigit)) {
                try {
                    cmd.level = std::stoi(level_str);
                    cmd.level = std::clamp(cmd.level, 1, 9);
                } catch (...) {
                }
            }
        }
    } else if (prefix == "-a") {
        cmd.type = Command::Add;
        cmd.append = true;
        cmd.level = 3;
        
        if (arg.length() > 2) {
            std::string level_str = arg.substr(2);
            if (!level_str.empty() && std::all_of(level_str.begin(), level_str.end(), ::isdigit)) {
                try {
                    cmd.level = std::stoi(level_str);
                    cmd.level = std::clamp(cmd.level, 1, 9);
                } catch (...) {
                }
            }
        }
    } else if (prefix == "-x") {
        cmd.type = Command::Extract;
    } else if (prefix == "-l") {
        cmd.type = Command::List;
    } else if (prefix == "-t") {
        cmd.type = Command::Test;
    } else if (prefix == "-d") {
        cmd.type = Command::Delete;
    } else {
        cmd.type = Command::None;
        return cmd;
    }
    
    for (int i = 2; i < argc; ++i) {
        std::string val = argv[i];
        
        if (val == "--sfx") {
            cmd.sfx = true;
        } else if (val == "--flat") {
            cmd.flat = true;
        } else if (val == "--force") {
            cmd.force = true;
        } else if (cmd.archive.empty()) {
            cmd.archive = val;
        } else {
            cmd.files.push_back(val);
        }
    }
    
    return cmd;
}

static int run_command(const Command& cmd) {
    using namespace std::chrono;
    
    auto start = steady_clock::now();
    int result = 0;
    
    switch (cmd.type) {
        case Command::Help:
            UI::show_help();
            return 0;
            
        case Command::Version:
            std::cout << "TARC STRIKE v2.00_OpenAi\n";
            std::cout << "Build: " << __DATE__ << " " << __TIME__ << "\n";
            return 0;
            
        case Command::License:
            UI::show_license();
            return 0;
            
        case Command::Create:
        case Command::Add: {
            if (cmd.archive.empty()) {
                UI::print_error("Specify archive name.");
                return 1;
            }
            
            if (cmd.files.empty()) {
                UI::print_error("No files or directories specified.");
                return 1;
            }
            
            std::string arch = IO::ensure_ext(cmd.archive);
            
            UI::print_info("Processing " + std::to_string(cmd.files.size()) + " items...");
            
            auto res = Engine::compress(arch, cmd.files, cmd.append, cmd.level);
            UI::print_summary(res, cmd.append ? "Add" : "Create");
            
            if (res.ok && cmd.sfx) {
                std::string sfx_exe = arch.substr(0, arch.find_last_of('.')) + ".exe";
                auto sfx_res = Engine::create_sfx(arch, sfx_exe);
                if (sfx_res.ok) {
                    UI::print_success("SFX archive created: " + sfx_exe);
                } else {
                    UI::print_error(sfx_res.message);
                }
            }
            
            result = res.ok ? 0 : 1;
            break;
        }
            
        case Command::Extract: {
            if (cmd.archive.empty()) {
                UI::print_error("Specify archive name.");
                return 1;
            }
            
            std::string arch = IO::ensure_ext(cmd.archive);
            
            auto res = Engine::extract(arch, cmd.filters, false, 0, cmd.flat);
            UI::print_summary(res, "Extract");
            result = res.ok ? 0 : 1;
            break;
        }
            
        case Command::Test: {
            if (cmd.archive.empty()) {
                UI::print_error("Specify archive name.");
                return 1;
            }
            
            std::string arch = IO::ensure_ext(cmd.archive);
            
            auto res = Engine::extract(arch, {}, true, 0, false);
            UI::print_summary(res, "Test");
            
            if (!res.ok || res.bytes_out == 0) {
                UI::print_error("Archive integrity check failed.");
                result = 1;
            }
            break;
        }
            
        case Command::List: {
            if (cmd.archive.empty()) {
                UI::print_error("Specify archive name.");
                return 1;
            }
            
            std::string arch = IO::ensure_ext(cmd.archive);
            
            auto res = Engine::list(arch);
            result = res.ok ? 0 : 1;
            break;
        }
            
        case Command::Delete: {
            UI::print_error("Delete operation not supported.");
            return 1;
        }
            
        case Command::None: {
            UI::print_error("Unknown command.");
            UI::show_help();
            return 1;
        }
    }
    
    auto elapsed = duration_cast<milliseconds>(steady_clock::now() - start);
    if (result == 0) {
        std::cout << Color::DIM << "Completed in " << UI::format_duration(elapsed) << Color::RESET << "\n";
    }
    
    return result;
}

int main(int argc, char* argv[]) {
    UI::enable_vtp();
    
    License::check_and_activate();
    
    Command cmd = parse_args(argc, argv);
    
    if (cmd.type == Command::Help && argc < 2) {
        UI::show_banner();
        UI::show_help();
        return 0;
    }
    
    UI::show_banner();
    
    return run_command(cmd);
}