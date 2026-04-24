#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <csignal>
#include <cstring>
#include "engine.h"
#include "ui.h"
#include "types.h"

#ifdef _WIN32
    #include <windows.h>
#endif

namespace fs = std::filesystem;

static constexpr const char* TARC_MAGIC_STR = "TRC2";

class SfxApp {
public:
    SfxApp() = default;
    
    int run(int argc, char* argv[]) {
        UI::enable_vtp();
        show_header();
        
        char path[MAX_PATH];
        if (GetModuleFileNameA(nullptr, path, MAX_PATH) == 0) {
            UI::print_error("Cannot determine executable path.");
            return show_prompt();
        }
        
        self_path_ = path;
        archive_offset_ = find_archive_offset();
        
        if (archive_offset_ == 0) {
            UI::print_error("No TARC2 archive data found. This is not a valid SFX archive.");
            return show_prompt();
        }
        
        return show_menu();
    }
    
private:
    std::string self_path_;
    size_t archive_offset_ = 0;
    
    void show_header() const {
        std::cout << Color::CYAN << "╔═══════════════════════════════════════════╗\n";
        std::cout << "║     TARC SELF-EXTRACTOR  v2.00_OpenAi    ║\n";
        std::cout << "║     Powered by Strike Engine           ║\n";
        std::cout << "╚═══════════════════════════════════════════╝" << Color::RESET << "\n\n";
        
        std::cout << Color::DIM << "Archive: " << Color::RESET << self_path_ << "\n";
        std::cout << Color::DIM << "Offset:  " << Color::RESET << archive_offset_ << " bytes\n\n";
    }
    
    size_t find_archive_offset() const {
        std::ifstream f(self_path_, std::ios::binary | std::ios::ate);
        if (!f) return 0;
        
        size_t file_size = static_cast<size_t>(f.tellg());
        if (file_size < 4) return 0;
        
        std::vector<char> buffer(file_size);
        f.seekg(0, std::ios::beg);
        f.read(buffer.data(), file_size);
        
        for (size_t i = file_size - 4; i > 1024 * 100; --i) {
            if (buffer[i] == 'T' && buffer[i+1] == 'R' && 
                buffer[i+2] == 'C' && buffer[i+3] == '2') {
                return i;
            }
        }
        return 0;
    }
    
    int show_menu() const {
        std::cout << Color::BOLD << "OPERATIONS:" << Color::RESET << "\n";
        std::cout << "  " << Color::GREEN << "[1]" << Color::RESET << " Extract all files\n";
        std::cout << "  " << Color::CYAN << "[2]" << Color::RESET << " List contents\n";
        std::cout << "  " << Color::YELLOW << "[3]" << Color::RESET << " Verify integrity\n";
        std::cout << "  " << Color::RED << "[4]" << Color::RESET << " Exit\n\n";
        
        std::cout << "Select option> ";
        
        std::string input;
        std::getline(std::cin, input);
        if (input.empty()) return 0;
        
        char choice = input[0];
        
        TarcResult res;
        switch (choice) {
            case '1':
                return extract_all();
            case '2':
                return list_contents();
            case '3':
                return verify_integrity();
            case '4':
                return 0;
            default:
                UI::print_warning("Invalid option.");
                return show_menu();
        }
    }
    
    int extract_all() const {
        std::cout << "\n";
        UI::print_info("Extracting files...");
        
        auto res = Engine::extract(self_path_, {}, false, archive_offset_);
        UI::print_summary(res, "SFX Extract");
        
        if (!res.ok) {
            UI::print_error("Extraction failed: " + res.message);
        }
        
        return show_prompt();
    }
    
    int list_contents() const {
        std::cout << "\n";
        UI::print_info("Archive contents:");
        
        auto res = Engine::list(self_path_, archive_offset_);
        
        if (!res.ok) {
            UI::print_error("List failed: " + res.message);
        }
        
        return show_prompt();
    }
    
    int verify_integrity() const {
        std::cout << "\n";
        UI::print_info("Verifying integrity...");
        
        auto res = Engine::extract(self_path_, {}, true, archive_offset_);
        UI::print_summary(res, "SFX Test");
        
        if (!res.ok || res.bytes_out == 0) {
            UI::print_error("Integrity check FAILED.");
        } else {
            UI::print_success("Integrity check PASSED.");
        }
        
        return show_prompt();
    }
    
    int show_prompt() const {
        std::cout << "\n" << Color::DIM << "Press Enter to exit..." << Color::RESET;
        std::string line;
        std::getline(std::cin, line);
        return 0;
    }
};

#ifdef _WIN32
    BOOL WINAPI CtrlHandler(DWORD dwCtrlType) {
        if (dwCtrlType == CTRL_C_EVENT) {
            std::cout << Color::YELLOW << "\nOperation cancelled by user." << Color::RESET << "\n";
            exit(1);
        }
        return FALSE;
    }
#endif

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleCtrlHandler(CtrlHandler, TRUE);
#endif
    
    SfxApp app;
    return app.run(argc, argv);
}