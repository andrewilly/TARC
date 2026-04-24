#include "ui.h"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <chrono>
#include <array>
#include <cstdio>
#include <thread>
#include <mutex>

#ifdef _WIN32
    #include <windows.h>
#endif

namespace {

std::mutex cout_mutex;

void safe_print(const std::string& s) {
    std::lock_guard<std::mutex> lock(cout_mutex);
    std::cout << s << std::flush;
}

}

namespace UI {

void enable_vtp() {
#ifdef _WIN32
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE) {
        DWORD dwMode = 0;
        if (GetConsoleMode(hOut, &dwMode)) {
            dwMode |= 0x0004;
            SetConsoleMode(hOut, dwMode);
        }
    }
    SetConsoleOutputCP(65001);
#endif
}

void disable_vtp() {
#ifdef _WIN32
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE) {
        DWORD dwMode = 0;
        if (GetConsoleMode(hOut, &dwMode)) {
            dwMode &= ~0x0004;
            SetConsoleMode(hOut, dwMode);
        }
    }
#endif
}

void show_banner() {
    std::cout << Color::CYAN << Color::BOLD
              << "                       TARC STRIKE v2.00_OpenAi\n"
              << "                  Advanced Solid Compression Tool\n"
              << "                       (c) 2026 Andre Willy Rizzo\n"
              << Color::RESET << "\n";
}

void show_help() {
    std::cout << Color::BOLD << "Usage: " << Color::BRIGHT_WHITE << "tarc [command] [options] archive [files...]" << Color::RESET << "\n\n";
    
    std::cout << Color::BOLD << "Commands:" << Color::RESET << "\n";
    std::cout << "  " << Color::GREEN << "-c" << Color::RESET << " [level]  Create archive (level 1-9, default 3)\n";
    std::cout << "  " << Color::GREEN << "-a" << Color::RESET << " [level]  Add files to existing archive\n";
    std::cout << "  " << Color::YELLOW << "-x" << Color::RESET << " [filt]  Extract files (supports wildcards)\n";
    std::cout << "  " << Color::CYAN << "-l" << Color::RESET << "          List archive contents\n";
    std::cout << "  " << Color::MAGENTA << "-t" << Color::RESET << "          Test archive integrity\n";
    std::cout << "  " << Color::RED << "-d" << Color::RESET << " [files]  Delete files from archive\n";
    
    std::cout << "\n" << Color::BOLD << "Compression Levels:" << Color::RESET << "\n";
    std::cout << "  " << Color::GREEN << "-cbest" << Color::RESET << "    Maximum compression (LZMA level 9)\n";
    std::cout << "  " << Color::GREEN << "-cfast" << Color::RESET << "    Fastest compression (ZSTD)\n";
    std::cout << "  " << Color::GREEN << "-c[N]" << Color::RESET << "       Level N (1=speed, 9=ratio)\n";
    
    std::cout << "\n" << Color::BOLD << "Options:" << Color::RESET << "\n";
    std::cout << "  " << Color::WHITE << "--sfx" << Color::RESET << "       Create self-extracting archive\n";
    std::cout << "  " << Color::WHITE << "--flat" << Color::RESET << "      Flat extraction (no paths)\n";
    std::cout << "  " << Color::WHITE << "--force" << Color::RESET << "    Overwrite existing files\n";
    
    std::cout << "\n" << Color::BOLD << "Features:" << Color::RESET << "\n";
    std::cout << "  • Solid blocks (256MB) for maximum ratio\n";
    std::cout << "  • Deduplication via XXH64 checksums\n";
    std::cout << "  • Smart codec selection (LZMA/ZSTD/STORE)\n";
    std::cout << "  • Windows native I/O for best performance\n";
    
    std::cout << "\n" << Color::DIM << "Type 'tarc --license' for license information.\n" << Color::RESET;
}

void show_license() {
    std::cout << Color::CYAN << "═══════════════════════════════════════════════════════════════════\n";
    std::cout << "                     TARC LICENSE                        \n";
    std::cout << "═══════════════════════════════════════════════════════════════════" << Color::RESET << "\n\n";
    std::cout << "TARC STRIKE v2.00_OpenAi\n";
    std::cout << "Copyright (C) 2026 André Willy Rizzo\n\n";
    std::cout << "This software is provided AS IS, without warranty of any kind.\n";
    std::cout << Color::DIM << "TARC comes with ABSOLUTELY NO WARRANTY." << Color::RESET << "\n\n";
}

std::string human_size(uint64_t b) {
    std::array<const char*, 5> units = {"B", "KB", "MB", "GB", "TB"};
    size_t unit_idx = 0;
    double size = static_cast<double>(b);
    
    while (size >= 1024.0 && unit_idx < units.size() - 1) {
        size /= 1024.0;
        unit_idx++;
    }
    
    char buf[32];
    if (unit_idx == 0) {
        snprintf(buf, sizeof(buf), "%llu %s", static_cast<unsigned long long>(b), units[unit_idx]);
    } else {
        snprintf(buf, sizeof(buf), "%.2f %s", size, units[unit_idx]);
    }
    return std::string(buf);
}

std::string compress_ratio(uint64_t orig, uint64_t comp) {
    if (orig == 0) return "  -  ";
    
    double ratio = 100.0 * (1.0 - static_cast<double>(comp) / static_cast<double>(orig));
    if (ratio < 0) ratio = 0;
    
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1f%%", ratio);
    return std::string(buf);
}

std::string format_duration(const std::chrono::milliseconds& ms) {
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(ms);
    auto mins = std::chrono::duration_cast<std::chrono::minutes>(secs);
    
    char buf[32];
    if (mins.count() > 0) {
        snprintf(buf, sizeof(buf), "%02ldm %02lds", static_cast<long>(mins.count() % 60), static_cast<long>(secs.count() % 60));
    } else if (secs.count() > 0) {
        snprintf(buf, sizeof(buf), "%lds", static_cast<long>(secs.count()));
    } else {
        snprintf(buf, sizeof(buf), "%lldms", static_cast<long long>(ms.count()));
    }
    return std::string(buf);
}

void print_info(const std::string& msg) {
    std::cout << Color::CYAN << "ℹ" << Color::RESET << "  " << msg << "\n";
}

void print_warning(const std::string& msg) {
    std::cout << Color::YELLOW << "⚠" << Color::RESET << "  " << msg << "\n";
}

void print_error(const std::string& msg) {
    std::cerr << Color::RED << "✖" << Color::RESET << "  ERROR: " << msg << "\n";
}

void print_success(const std::string& msg) {
    std::cout << Color::GREEN << "✔" << Color::RESET << "  " << msg << "\n";
}

void print_progress(size_t current, size_t total, const std::string& current_file) {
    if (total == 0) return;
    
    float percent = static_cast<float>(current) / total * 100.0f;
    int width = 25;
    int pos = static_cast<int>(width * current / total);
    
    std::string short_name = current_file;
    if (short_name.length() > 30) {
        short_name = "..." + short_name.substr(short_name.length() - 27);
    }
    
    std::lock_guard<std::mutex> lock(cout_mutex);
    std::cout << "\r" << Color::CYAN << " [" << Color::BOLD;
    for (int i = 0; i < width; ++i) {
        if (i < pos) std::cout << "=";
        else if (i == pos) std::cout << ">";
        else std::cout << " ";
    }
    std::cout << Color::RESET << Color::CYAN << "] " 
              << std::fixed << std::setprecision(1) << percent << "% "
              << Color::DIM << short_name << std::flush;
}

void print_progress_end() {
    std::cout << "\n" << Color::RESET;
}

void print_add(const std::string& name, uint64_t size, Codec codec, float ratio) {
    bool is_dedup = (ratio >= 1.0f);
    
    std::cout << "\n" << Color::GREEN << "[+]" << Color::RESET << " ["
              << Color::YELLOW << std::setw(5) << codec_name(codec) << Color::RESET << "] "
              << std::left << std::setw(40) << name.substr(0, 40) << " "
              << std::right << std::setw(10) << human_size(size) << "  "
              << Color::DIM << (is_dedup ? "→ DEDUP" : compress_ratio(size, static_cast<uint64_t>(size * (1.0f - ratio)))) 
              << Color::RESET << "\n";
}

void print_extract(const std::string& name, uint64_t size, bool test, bool ok) {
    if (!ok) {
        std::cout << Color::RED << "[✖]" << Color::RESET << " " << name << "\n";
        return;
    }
    std::cout << Color::CYAN << "[" << (test ? "OK" : "×") << "]" << Color::RESET << " "
              << std::left << std::setw(42) << name.substr(0, 42) << " "
              << std::right << std::setw(10) << human_size(size) << "\n";
}

void print_delete(const std::string& name) {
    std::cout << Color::RED << "[-] " << Color::RESET << name << "\n";
}

void print_list_entry(const std::string& name, uint64_t orig, uint64_t comp, Codec codec) {
    bool is_duplicate = (comp == 0);
    
    std::cout << "  [" << Color::YELLOW << std::setw(5) << codec_name(codec) << Color::RESET << "] "
              << std::left << std::setw(42) << name.substr(0, 42) << " "
              << std::right << std::setw(10) << human_size(orig) << "  "
              << Color::DIM 
              << (is_duplicate ? "(DUPLICATE)" : compress_ratio(orig, is_duplicate ? 0 : orig))
              << Color::RESET << "\n";
}

void print_summary(const TarcResult& result, const std::string& op) {
    std::cout << "\n";
    
    if (!result.ok) {
        std::cerr << Color::RED << "✖ " << op << " failed: " << result.message << Color::RESET << "\n";
        if (result.error != TarcError::None) {
            std::cerr << Color::DIM << "  Code: " << error_message(result.error) << Color::RESET << "\n";
        }
        return;
    }
    
    if (result.bytes_in > 0 && result.bytes_out > 0) {
        std::cout << Color::GREEN << "✔ " << op << " completed successfully." << Color::RESET << "\n";
        std::cout << "  " << human_size(result.bytes_in) << " → " << human_size(result.bytes_out) << "  "
                  << Color::DIM << "(" << compress_ratio(result.bytes_in, result.bytes_out) << ")" << Color::RESET << "\n";
        
        for (const auto& warn : result.warnings) {
            std::cout << Color::YELLOW << "  ⚠ " << warn << Color::RESET << "\n";
        }
    } else {
        std::cout << Color::GREEN << "✔ " << op << " completed successfully." << Color::RESET << "\n";
    }
}

void print_spinner(size_t step) {
    static const char* chars = "|/-\\";
    std::cout << "\r" << chars[step % 4] << std::flush;
}

void print_table_row(const std::vector<std::string>& cols, const std::vector<size_t>& widths) {
    for (size_t i = 0; i < cols.size(); ++i) {
        if (i > 0) std::cout << "  ";
        std::cout << std::left << std::setw(widths[i]) << cols[i];
    }
    std::cout << "\n";
}

UI::ProgressBar::ProgressBar(size_t total, const std::string& label)
    : total_(total), current_(0), label_(label), active_(true) {
    update(0);
}

UI::ProgressBar::~ProgressBar() {
    finish();
}

void UI::ProgressBar::set_label(const std::string& label) {
    label_ = label;
}

void UI::ProgressBar::update(size_t current, const std::string& status) {
    current_ = current;
    if (!active_) return;
    
    float pct = total_ > 0 ? static_cast<float>(current) / total_ * 100.0f : 100.0f;
    int bar_width = 20;
    int pos = static_cast<int>(bar_width * current / total_);
    
    std::lock_guard<std::mutex> lock(cout_mutex);
    std::cout << "\r" << Color::CYAN << label_ << " [";
    for (int i = 0; i < bar_width; ++i) {
        std::cout << (i < pos ? "█" : "░");
    }
    std::cout << Color::RESET << "] " << std::fixed << std::setprecision(1) << pct << "%";
    if (!status.empty()) {
        std::cout << " " << Color::DIM << status << Color::RESET;
    }
    std::cout << std::flush;
}

void UI::ProgressBar::finish() {
    if (!active_) return;
    active_ = false;
    update(total_);
    std::cout << "\n" << Color::RESET;
}

UI::Spinner::Spinner(const std::string& message)
    : message_(message), step_(0), active_(true) {
    std::cout << message_ << " " << std::flush;
}

UI::Spinner::~Spinner() {
    if (active_) {
        std::cout << "\r" << std::string(message_.length() + 2, ' ') << "\r";
    }
}

void UI::Spinner::spin() {
    if (!active_) return;
    static const char* chars = "⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏";
    step_ = (step_ + 1) % 10;
    std::cout << "\r" << message_ << " " << chars[step_] << std::flush;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

void UI::Spinner::finish(bool success, const std::string& message) {
    active_ = false;
    std::lock_guard<std::mutex> lock(cout_mutex);
    std::cout << "\r" << message_ << " ";
    if (success) {
        std::cout << Color::GREEN << "✔" << Color::RESET;
    } else {
        std::cout << Color::RED << "✖" << Color::RESET;
    }
    if (!message.empty()) {
        std::cout << " " << message;
    }
    std::cout << "\n";
}

}