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
#include <exception>

namespace fs = std::filesystem;

class ProgressReporter : public ProgressCallback {
public:
    size_t current = 0;
    size_t total = 0;
    std::string current_file;
    bool cancelled = false;

    void on_progress(size_t c, size_t t, const std::string& file) override {
        current = c;
        total = t;
        current_file = file;
        UI::print_progress(current, total, current_file);
    }

    void on_warning(const std::string& msg) override {
        std::cout << "\n";
        UI::print_warning(msg);
    }

    bool is_cancelled() const override {
        return cancelled;
    }
};

struct Command {
    enum Type {
        None,
        Create,
        Extract,
        List,
        Test,
        Help,
        License,
        Version
    } type = None;
    
    int level = 7;
    bool sfx = false;
    bool flat = false;
    bool force = false;
    bool verify = true;  // default: verify integrity
    int threads = 0;     // 0 = auto
    std::string output_dir;  // FEATURE #4: --output-dir
    Codec codec_override = Codec::LZMA;  // FEATURE #6: codec override
    bool has_codec_override = false;      // true se l'utente ha specificato --zstd/--lz4/etc.
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
        cmd.level = 7;
        
        if (arg == "-cbest") {
            cmd.level = 19;  // Massima compressione: LZMA2 1GB dict + ZSTD 19 ultra
        } else if (arg == "-cfast") {
            cmd.level = 1;
        } else if (arg.length() > 2) {
            std::string level_str = arg.substr(2);
            if (!level_str.empty() && std::all_of(level_str.begin(), level_str.end(), ::isdigit)) {
                try {
                    cmd.level = std::stoi(level_str);
                    cmd.level = std::clamp(cmd.level, 1, 19);  // Livelli 1-19
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
        } else if (val == "--verify") {
            cmd.verify = true;
        } else if (val == "--no-verify") {
            cmd.verify = false;
        } else if (val == "--output-dir" && i + 1 < argc) {
            cmd.output_dir = argv[++i];
        } else if (val == "--threads" && i + 1 < argc) {
            try {
                cmd.threads = std::clamp(std::stoi(argv[++i]), 1, 64);
            } catch (...) {
                cmd.threads = 0;
            }
        } else if (val == "--zstd") {
            cmd.codec_override = Codec::ZSTD;
            cmd.has_codec_override = true;
        } else if (val == "--lzma") {
            cmd.codec_override = Codec::LZMA;
            cmd.has_codec_override = true;
        } else if (val == "--lz4") {
            cmd.codec_override = Codec::LZ4;
            cmd.has_codec_override = true;
        } else if (val == "--brotli") {
            cmd.codec_override = Codec::BR;
            cmd.has_codec_override = true;
        } else if (val == "--store") {
            cmd.codec_override = Codec::STORE;
            cmd.has_codec_override = true;
        } else if (cmd.archive.empty()) {
            cmd.archive = val;
        } else {
            // Per estrazione/lista, i parametri sono filtri, non file
            if (cmd.type == Command::Extract || cmd.type == Command::List || cmd.type == Command::Test) {
                cmd.filters.push_back(val);
            } else {
                cmd.files.push_back(val);
            }
        }
    }
    
    return cmd;
}

static int run_command(const Command& cmd) {
    using namespace std::chrono;
    
    auto start = TarcUtil::safe_now();
    int result = 0;
    
    switch (cmd.type) {
        case Command::Help:
            UI::show_help();
            return 0;
            
        case Command::Version:
            std::cout << "TARC STRIKE v2.10_OpenAi\n";
            std::cout << "Build: " << __DATE__ << " " << __TIME__ << "\n";
            return 0;
            
        case Command::License:
            UI::show_license();
            return 0;
            
        case Command::Create: {
            if (cmd.archive.empty()) {
                UI::print_error("Specify archive name.");
                return 1;
            }
            
            if (cmd.files.empty()) {
                UI::print_error("No files or directories specified.");
                return 1;
            }
            
            std::string arch = IO::ensure_ext(cmd.archive);
            
            ProgressReporter reporter;
            Engine::set_progress_callback(&reporter);
            
            auto cmd_start = TarcUtil::safe_now();
            CompressOptions copts;
            copts.level = cmd.level;
            if (cmd.has_codec_override) {
                copts.codec = cmd.codec_override;
            }
            auto res = Engine::compress(arch, cmd.files, copts);
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                TarcUtil::safe_now() - cmd_start
            );
            
            UI::print_progress_end();
            UI::print_summary(res, "Create", elapsed);
            
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
            
            ProgressReporter reporter;
            Engine::set_progress_callback(&reporter);
            
            auto cmd_start = TarcUtil::safe_now();
            ExtractOptions xopts;
            xopts.test_only = false;
            xopts.flat_mode = cmd.flat;
            xopts.verify = cmd.verify;
            xopts.overwrite = cmd.force;
            xopts.output_dir = cmd.output_dir;
            auto res = Engine::extract(arch, cmd.filters, xopts);
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                TarcUtil::safe_now() - cmd_start
            );
            
            UI::print_progress_end();
            UI::print_summary(res, "Extract", elapsed);
            result = res.ok ? 0 : 1;
            break;
        }
        
        case Command::Test: {
            if (cmd.archive.empty()) {
                UI::print_error("Specify archive name.");
                return 1;
            }
            
            std::string arch = IO::ensure_ext(cmd.archive);
            
            ProgressReporter reporter;
            Engine::set_progress_callback(&reporter);
            
            auto cmd_start = TarcUtil::safe_now();
            ExtractOptions topts;
            topts.test_only = true;
            topts.verify = cmd.verify;
            auto res = Engine::extract(arch, {}, topts);
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                TarcUtil::safe_now() - cmd_start
            );
            
            UI::print_progress_end();
            UI::print_summary(res, "Test", elapsed);
            
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
            
        case Command::None: {
            UI::print_error("Unknown command.");
            UI::show_help();
            return 1;
        }
    }
    
    auto elapsed = duration_cast<milliseconds>(TarcUtil::safe_now() - start);
    if (result == 0) {
        std::cout << Color::DIM << "Completed in " << UI::format_duration(elapsed) << Color::RESET << "\n";
    }
    
    return result;
}

int main(int argc, char* argv[]) {
    UI::enable_vtp();

    // ====================================================================
    // SFX MODE: se tarc.exe ha un trailer SFX, auto-estrae.
    // Questo permette di usare tarc.exe come stub per archivi autoestraenti
    // senza bisogno di un eseguibile separato.
    // ====================================================================
    std::string self_path = IO::get_self_path();
    if (self_path.empty() && argc > 0) {
        self_path = argv[0];
    }

    if (!self_path.empty() && Engine::is_sfx_mode(self_path)) {
        // Siamo un archivio SFX — parse opzioni SFX minime
        std::string output_dir;
        bool overwrite = false;

        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--output-dir" && i + 1 < argc) {
                output_dir = argv[++i];
            } else if (arg == "--force" || arg == "-y") {
                overwrite = true;
            } else if (arg == "--help" || arg == "-h") {
                std::cout << Color::CYAN << Color::BOLD
                          << "  TARC STRIKE — Self-Extracting Archive\n"
                          << Color::RESET << "\n";
                std::cout << "Usage: " << Color::WHITE
                          << fs::path(argv[0]).filename().string() << " [options]"
                          << Color::RESET << "\n\n";
                std::cout << "Options:\n";
                std::cout << "  " << Color::WHITE << "--output-dir <path>"
                          << Color::RESET << "  Extract to directory (default: current)\n";
                std::cout << "  " << Color::WHITE << "--force, -y"
                          << Color::RESET << "          Overwrite existing files\n";
                std::cout << "  " << Color::WHITE << "--help, -h"
                          << Color::RESET << "          Show this help\n";
                return 0;
            }
        }

        // Banner SFX
        std::cout << Color::CYAN << Color::BOLD
                  << "  TARC STRIKE — Self-Extracting Archive\n"
                  << Color::RESET << "\n";

        // Auto-estrai
        ProgressReporter reporter;
        Engine::set_progress_callback(&reporter);

        auto sfx_start = TarcUtil::safe_now();
        auto res = Engine::extract_sfx(self_path, output_dir, overwrite);
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            TarcUtil::safe_now() - sfx_start
        );

        UI::print_progress_end();
        UI::print_summary(res, "SFX Extract", elapsed);

        if (!output_dir.empty()) {
            std::cout << Color::DIM << "  Output: " << output_dir << Color::RESET << "\n";
        }

        return res.ok ? 0 : 1;
    }

    // ====================================================================
    // NORMAL MODE: tarc.exe come archiviatore standard
    // ====================================================================
    bool show_license_full = false;
    Command cmd = parse_args(argc, argv);
    
    if (cmd.type == Command::License) {
        show_license_full = true;
    }
    
    License::check_and_activate(show_license_full);
    
    if (cmd.type == Command::Help && argc < 2) {
        UI::show_banner();
        UI::show_help();
        return 0;
    }
    
    if (cmd.type == Command::License) {
        UI::show_license();
        return 0;
    }
    
    UI::show_banner();
    
    try {
        return run_command(cmd);
    } catch (const std::bad_alloc& e) {
        std::cerr << Color::RED << "\n[FATAL] Out of memory: " << e.what() << Color::RESET << "\n";
        std::cerr << Color::DIM << "The input is too large for the available RAM." << Color::RESET << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << Color::RED << "\n[FATAL] " << e.what() << Color::RESET << "\n";
        return 1;
    } catch (...) {
        std::cerr << Color::RED << "\n[FATAL] Unknown error" << Color::RESET << "\n";
        return 1;
    }
}
