#include "ui.h"
#include "io.h"
#include "engine.h"
#include "types.h"

#include <iostream>
#include <fstream>
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
        Version
    } type = None;
    
    int level = 7;
    bool sfx = false;
    bool flat = false;
    bool force = false;
    bool verify = true;
    int threads = 0;
    std::string output_dir;
    Codec codec_override = Codec::LZMA;
    bool has_codec_override = false;
    std::string archive;
    std::vector<std::string> files;
    std::vector<std::string> filters;
    std::string help_topic; // for contextual help: "create", "extract", etc.
};

static Command parse_args(int argc, char* argv[]) {
    Command cmd;
    
    if (argc < 2) {
        cmd.type = Command::None;
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
        cmd.type = Command::Version;
        cmd.help_topic = "license";
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
        
        if (val == "--help" || val == "-h") {
            if (cmd.type == Command::Create) cmd.help_topic = "create";
            else if (cmd.type == Command::Extract) cmd.help_topic = "extract";
            else if (cmd.type == Command::List) cmd.help_topic = "list";
            else if (cmd.type == Command::Test) cmd.help_topic = "test";
            cmd.type = Command::Help;
            continue;
        }
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
            // For extract/list, parameters are filters, not files
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
            copts.threads = cmd.threads;
            if (cmd.has_codec_override) {
                copts.codec = cmd.codec_override;
                copts.has_codec_override = true;
            }
            auto res = Engine::compress(arch, cmd.files, copts);
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                TarcUtil::safe_now() - cmd_start
            );
            
            UI::print_progress_end();
            UI::print_summary(res, "Create", elapsed);
            
            if (res.ok && cmd.sfx) {
#ifdef _WIN32
                std::string sfx_ext = ".exe";
#else
                std::string sfx_ext = "";
#endif
                std::string sfx_exe = arch.substr(0, arch.find_last_of('.')) + sfx_ext;
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
        case Command::Help:
        case Command::Version:
            // handled in main() before run_command is reached
            return 0;
    }
    
    auto elapsed = duration_cast<milliseconds>(TarcUtil::safe_now() - start);
    if (result == 0) {
        std::cout << Color::DIM << "Completed in " << UI::format_duration(elapsed) << Color::RESET << "\n";
    }
    
    return result;
}

// ============================================================================
// SFX Helper Functions — Interactive menu, help, extract, list, test
// ============================================================================

// Show help for SFX mode
static void sfx_show_help(const char* exe_name) {
    std::string name = fs::path(exe_name).filename().string();
    std::cout << "\n";
    std::cout << Color::CYAN << Color::BOLD
              << "  TARC STRIKE - Self-Extracting Archive\n"
              << Color::RESET << "\n";
    std::cout << Color::BOLD << "Usage:\n" << Color::RESET << "\n";
    std::cout << "  " << Color::WHITE << name
              << Color::RESET << "                    Show interactive menu\n";
    std::cout << "  " << Color::WHITE << name << " --extract, -x"
              << Color::RESET << "       Extract archive\n";
    std::cout << "  " << Color::WHITE << name << " --list, -l"
              << Color::RESET << "          List archive contents\n";
    std::cout << "  " << Color::WHITE << name << " --test, -t"
              << Color::RESET << "          Test archive integrity\n";
    std::cout << "  " << Color::WHITE << name << " --help, -h"
              << Color::RESET << "          Show this help\n";
    std::cout << "\n" << Color::BOLD << "Options:\n" << Color::RESET << "\n";
    std::cout << "  " << Color::WHITE << "--output-dir <path>"
              << Color::RESET << "  Extract to directory (default: current)\n";
    std::cout << "  " << Color::WHITE << "--force, -y"
              << Color::RESET << "          Overwrite existing files\n";
    std::cout << "\n" << Color::DIM
              << "Examples:\n"
              << "  " << name << " --extract                 Extract to current dir\n"
              << "  " << name << " --extract --force         Overwrite without asking\n"
              << "  " << name << " --output-dir C:\\Temp      Extract to C:\\Temp\n"
              << "  " << name << " --list                    View archive contents\n"
              << "  " << name << " --test                    Test integrity\n"
              << Color::RESET << "\n";
}

// Read a line of input from stdin (cross-platform)
static std::string sfx_read_line() {
    std::string line;
    std::getline(std::cin, line);
    // Trim whitespace and convert to lowercase
    std::string result;
    for (char c : line) {
        if (c == '\r' || c == '\n') continue;
        result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return result;
}

// Extract the embedded SFX archive
static int sfx_do_extract(const std::string& self_path,
                           const std::string& output_dir, bool overwrite) {
    ProgressReporter reporter;
    Engine::set_progress_callback(&reporter);

    auto start = TarcUtil::safe_now();
    auto res = Engine::extract_sfx(self_path, output_dir, overwrite);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        TarcUtil::safe_now() - start
    );

    UI::print_progress_end();
    UI::print_summary(res, "SFX Extract", elapsed);

    if (!output_dir.empty()) {
        std::cout << Color::DIM << "  Output: " << output_dir << Color::RESET << "\n";
    }
    return res.ok ? 0 : 1;
}

// Extract the embedded SFX archive to a temporary file (shared utility)
static bool sfx_extract_to_temp(const std::string& self_path,
                                  const std::string& temp_archive_path) {
    SfxTrailer trailer;
    std::ifstream self(self_path, std::ios::binary);
    if (!self) {
        UI::print_error("Cannot open: " + self_path);
        return false;
    }

    std::ofstream out(temp_archive_path, std::ios::binary);
    if (!out) {
        UI::print_error("Cannot create temporary file.");
        return false;
    }

    self.clear();
    self.seekg(-static_cast<std::streamoff>(SFX_TRAILER_SIZE), std::ios::end);
    self.read(reinterpret_cast<char*>(&trailer), SFX_TRAILER_SIZE);
    if (std::memcmp(trailer.magic, SFX_MAGIC, 8) != 0) {
        UI::print_error("Invalid SFX archive.");
        return false;
    }

    self.clear();
    self.seekg(static_cast<std::streamoff>(trailer.archive_offset));

    const size_t BUF = 1024 * 1024;
    std::vector<char> buf(BUF);
    uint64_t remaining = trailer.archive_size;
    while (remaining > 0) {
        size_t to_read = static_cast<size_t>(std::min(remaining, static_cast<uint64_t>(BUF)));
        self.read(buf.data(), static_cast<std::streamsize>(to_read));
        out.write(buf.data(), static_cast<std::streamsize>(to_read));
        remaining -= to_read;
    }
    out.flush();
    out.close();
    return true;
}

// List contents of the SFX archive (extract to temp, list, cleanup)
static int sfx_do_list(const std::string& self_path) {
    fs::path temp_dir = fs::temp_directory_path();
    fs::path temp_archive = temp_dir / "tarc_sfx_list_temp.strk";

    // Extract the embedded archive to a temporary file
    if (!sfx_extract_to_temp(self_path, temp_archive.string())) {
        return 1;
    }

    // List the contents
    auto res = Engine::list(temp_archive.string());

    // Cleanup
    std::error_code ec;
    fs::remove(temp_archive, ec);

    return res.ok ? 0 : 1;
}

// Test integrity of the SFX archive (WITHOUT extracting files to disk)
static int sfx_do_test(const std::string& self_path) {
    fs::path temp_dir = fs::temp_directory_path();
    fs::path temp_archive = temp_dir / "tarc_sfx_test_temp.strk";

    // Extract the embedded archive to a temporary file
    if (!sfx_extract_to_temp(self_path, temp_archive.string())) {
        return 1;
    }

    // Test integrity with test_only=true (writes nothing to disk)
    ProgressReporter reporter;
    Engine::set_progress_callback(&reporter);

    auto start = TarcUtil::safe_now();
    ExtractOptions topts;
    topts.test_only = true;
    topts.verify = true;
    auto res = Engine::extract(temp_archive.string(), {}, topts);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        TarcUtil::safe_now() - start
    );

    UI::print_progress_end();
    UI::print_summary(res, "SFX Test", elapsed);

    // Cleanup
    std::error_code ec;
    fs::remove(temp_archive, ec);

    if (!res.ok || res.bytes_out == 0) {
        UI::print_error("Archive integrity check FAILED.");
        return 1;
    }

    UI::print_success("Archive integrity check PASSED.");
    return 0;
}

// SFX interactive menu
static int sfx_interactive_menu(const std::string& self_path,
                                 const std::string& output_dir, bool overwrite) {
    while (true) {
        // Banner + menu
        std::cout << "\n";
        std::cout << Color::CYAN << Color::BOLD
                  << "  ╔══════════════════════════════════════╗\n"
                  << "  ║      TARC STRIKE - SFX Archive       ║\n" // 6 spazi + 25 testo + 7 spazi = 38
                  << "  ╠══════════════════════════════════════╣\n"
                  << "  ║                                      ║\n"
                  << "  ║  " << Color::GREEN << "1" << Color::CYAN << Color::BOLD << ") Extract archive"
                  << std::string(18, ' ') << "║\n" // 2 + 1 + 2 + 15 = 20. 38-20 = 18
                  << "  ║  " << Color::GREEN << "2" << Color::CYAN << Color::BOLD << ") View contents"
                  << std::string(20, ' ') << "║\n" // 2 + 1 + 2 + 13 = 18. 38-18 = 20
                  << "  ║  " << Color::GREEN << "3" << Color::CYAN << Color::BOLD << ") Test integrity"
                  << std::string(19, ' ') << "║\n" // 2 + 1 + 2 + 14 = 19. 38-19 = 19
                  << "  ║  " << Color::GREEN << "4" << Color::CYAN << Color::BOLD << ") Help"
                  << std::string(29, ' ') << "║\n" // 2 + 1 + 2 + 4  = 9.  38-9  = 29
                  << "  ║  " << Color::RED   << "0" << Color::CYAN << Color::BOLD << ") Exit"
                  << std::string(29, ' ') << "║\n" // 2 + 1 + 2 + 4  = 9.  38-9  = 29
                  << "  ║                                      ║\n"
                  << "  ╚══════════════════════════════════════╝"
                  << Color::RESET << "\n";

        std::cout << Color::BOLD << "  Choice: " << Color::RESET;
        std::cout.flush();

        std::string choice = sfx_read_line();

        if (choice == "1" || choice == "e" || choice == "extract" || choice == "x") {
            std::cout << "\n";
            int result = sfx_do_extract(self_path, output_dir, overwrite);
            if (result == 0) {
                UI::print_success("Extraction completed.");
            }
        } else if (choice == "2" || choice == "l" || choice == "list" || choice == "v") {
            std::cout << "\n";
            sfx_do_list(self_path);
            std::cout << "\n" << Color::DIM << "  Press Enter to continue..." << Color::RESET;
            sfx_read_line();
        } else if (choice == "3" || choice == "t" || choice == "test") {
            std::cout << "\n";
            sfx_do_test(self_path);
            std::cout << "\n" << Color::DIM << "  Press Enter to continue..." << Color::RESET;
            sfx_read_line();
        } else if (choice == "4" || choice == "h" || choice == "help" || choice == "?") {
            sfx_show_help(self_path.c_str());
            std::cout << Color::DIM << "  Press Enter to continue..." << Color::RESET;
            sfx_read_line();
        } else if (choice == "0" || choice == "q" || choice == "quit" || choice == "exit") {
            std::cout << Color::DIM << "\n  Goodbye.\n" << Color::RESET;
            return 0;
        } else {
            std::cout << Color::YELLOW << "  Invalid choice. Type a number (0-4) or a command.\n" << Color::RESET;
        }
    }
}

int main(int argc, char* argv[]) {
    UI::enable_vtp();

    // ====================================================================
    // SFX MODE: if tarc.exe has an SFX trailer, auto-extract.
    // This allows using tarc.exe as a stub for self-extracting archives
    // without needing a separate executable.
    // ====================================================================
    std::string self_path = IO::get_self_path();
    if (self_path.empty() && argc > 0) {
        self_path = argv[0];
    }

    if (!self_path.empty() && Engine::is_sfx_mode(self_path)) {
        // ====================================================================
        // SFX MODE: self-extracting archive with interactive menu
        // ====================================================================

        // Parse command line options
        std::string output_dir;
        bool overwrite = false;
        bool silent_mode = false;  // true if a direct command was specified (no menu)
        enum SfxAction { None, Extract, List, Test, Help } action = None;

        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--output-dir" && i + 1 < argc) {
                output_dir = argv[++i];
            } else if (arg == "--force" || arg == "-y") {
                overwrite = true;
            } else if (arg == "--extract" || arg == "-x") {
                action = Extract; silent_mode = true;
            } else if (arg == "--list" || arg == "-l") {
                action = List; silent_mode = true;
            } else if (arg == "--test" || arg == "-t") {
                action = Test; silent_mode = true;
            } else if (arg == "--help" || arg == "-h") {
                action = Help; silent_mode = true;
            }
        }

        // ====================================================================
        // HELP: show full SFX help
        // ====================================================================
        if (action == Help) {
            sfx_show_help(argv[0]);
            return 0;
        }

        // ====================================================================
        // SILENT MODE: direct command from command line (no menu)
        // ====================================================================
        if (silent_mode) {
            ProgressReporter reporter;
            Engine::set_progress_callback(&reporter);

            if (action == Extract) {
                return sfx_do_extract(self_path, output_dir, overwrite);
            } else if (action == List) {
                return sfx_do_list(self_path);
            } else if (action == Test) {
                return sfx_do_test(self_path);
            }
            return 1;
        }

        // ====================================================================
        // INTERACTIVE MENU: show the menu and wait for user choice
        // ====================================================================
        return sfx_interactive_menu(self_path, output_dir, overwrite);
    }

    // ====================================================================
    // NORMAL MODE: tarc.exe as standard archiver
    // ====================================================================
    Command cmd = parse_args(argc, argv);
    
    if (cmd.type == Command::None && argc < 2) {
        UI::show_compact_help();
        return 0;
    }
    
    if (cmd.type == Command::Help) {
        if (cmd.help_topic.empty()) {
            UI::show_help();
        } else {
            if (cmd.help_topic == "create")   UI::show_help_create();
            else if (cmd.help_topic == "extract")  UI::show_help_extract();
            else if (cmd.help_topic == "list")     UI::show_help_list();
            else if (cmd.help_topic == "test")     UI::show_help_test();
            else UI::show_help();
        }
        return 0;
    }
    
    if (cmd.type == Command::Version) {
        if (cmd.help_topic == "license") {
            std::cout << "TARC STRIKE v2.10_OpenAi\n"
                      << "Copyright (c) 2026 Andre Willy Rizzo\n"
                      << "Open source software — see LICENSE for details.\n";
        } else {
            std::cout << "TARC STRIKE v2.10_OpenAi\n";
        }
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
