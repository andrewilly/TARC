#include "ui.h"
#include "simd_opt.h"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <chrono>
#include <array>
#include <cstdio>
#include <thread>
#include <mutex>
#include <memory>
#include <algorithm>

#ifdef _WIN32
    #define NOMINMAX
    #include <windows.h>
#else
    #include <sys/ioctl.h>
    #include <unistd.h>
#endif

namespace {

std::mutex cout_mutex;

void safe_print(const std::string& s) {
    std::lock_guard<std::mutex> lock(cout_mutex);
    std::cout << s << std::flush;
}

static int get_terminal_width() {
#ifdef _WIN32
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE) {
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        if (GetConsoleScreenBufferInfo(hOut, &csbi)) {
            return static_cast<int>(csbi.srWindow.Right - csbi.srWindow.Left + 1);
        }
    }
#else
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        return static_cast<int>(ws.ws_col);
    }
#endif
    return 80;
}

// Stores the last completed file info for display above the progress bar.
// print_add() and print_extract() write here; ProgressBar::update() renders it.
static std::string g_last_status_line;

// When true, the progress bar should render the status line above it.
static bool g_has_status_line = false;

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
              << "                       TARC STRIKE v2.10_OpenAi\n"
              << "                  Advanced Solid Compression Tool\n"
              << "                       (c) 2026 Andre Willy Rizzo\n"
              << Color::RESET << "\n";
    // Show active SIMD features
    std::string simd = SimdOpt::simd_info_string();
    std::cout << Color::DIM << "  SIMD: " << simd << Color::RESET << "\n";
}

void show_compact_help() {
    std::cout << Color::CYAN << Color::BOLD
              << "TARC STRIKE v2.10_OpenAi" << Color::RESET
              << " — Advanced Solid Compression Tool\n\n";
    std::cout << Color::BOLD << "Usage:" << Color::RESET << "\n";
    std::cout << "  " << Color::GREEN << "tarc -c" << Color::RESET << "[level] " << Color::WHITE << "<archive> <file>..." << Color::RESET << "   Create archive\n";
    std::cout << "  " << Color::YELLOW << "tarc -x" << Color::RESET << " " << Color::WHITE << "<archive>" << Color::RESET << " [filter]        Extract files\n";
    std::cout << "  " << Color::CYAN << "tarc -l" << Color::RESET << " " << Color::WHITE << "<archive>" << Color::RESET << "                 List contents\n";
    std::cout << "  " << Color::MAGENTA << "tarc -t" << Color::RESET << " " << Color::WHITE << "<archive>" << Color::RESET << "                 Test integrity\n\n";
    std::cout << Color::DIM << "Try 'tarc --help' for full help, or 'tarc -c --help' for command-specific help.\n" << Color::RESET;
}

void show_help() {
    std::cout << Color::BOLD << "Usage: " << Color::BRIGHT_WHITE << "tarc [command] [options] archive [files...]" << Color::RESET << "\n\n";
    
    std::cout << Color::BOLD << "Commands:" << Color::RESET << "\n";
    std::cout << "  " << Color::GREEN << "-c" << Color::RESET << " [level]  Create archive (level 1-19, default 7)\n";
    std::cout << "  " << Color::YELLOW << "-x" << Color::RESET << " [filt]  Extract files (supports wildcards)\n";
    std::cout << "  " << Color::CYAN << "-l" << Color::RESET << "          List archive contents\n";
    std::cout << "  " << Color::MAGENTA << "-t" << Color::RESET << "          Test archive integrity\n";
    std::cout << "  " << Color::DIM << "Use 'tarc -c --help' for detailed help on a specific command.\n" << Color::RESET;
    
    std::cout << "\n" << Color::BOLD << "Compression Levels:" << Color::RESET << "\n";
    std::cout << "  " << Color::GREEN << "-cbest" << Color::RESET << "    Maximum compression (level 19)\n";
    std::cout << "  " << Color::GREEN << "-cfast" << Color::RESET << "    Fastest compression (level 1)\n";
    std::cout << "  " << Color::GREEN << "-c[N]" << Color::RESET << "       Level N (1=speed, 19=ratio)\n";
    
    std::cout << "\n" << Color::BOLD << "Options:" << Color::RESET << "\n";
    std::cout << "  " << Color::WHITE << "--sfx" << Color::RESET << "           Create self-extracting archive\n";
    std::cout << "  " << Color::WHITE << "--flat" << Color::RESET << "          Flat extraction (no paths)\n";
    std::cout << "  " << Color::WHITE << "--force" << Color::RESET << "         Overwrite existing files\n";
    std::cout << "  " << Color::WHITE << "--verify" << Color::RESET << "        Verify integrity after operation\n";
    std::cout << "  " << Color::WHITE << "--no-verify" << Color::RESET << "     Skip integrity verification\n";
    std::cout << "  " << Color::WHITE << "--output-dir" << Color::RESET << " <path>  Extract to directory\n";
    std::cout << "  " << Color::WHITE << "--threads" << Color::RESET << " N     Set compression threads (default: auto)\n";
    std::cout << "  " << Color::WHITE << "--version, -v" << Color::RESET << "  Show version information\n";
    
    std::cout << "\n" << Color::BOLD << "Codec Override (create):" << Color::RESET << "\n";
    std::cout << "  " << Color::WHITE << "--zstd" << Color::RESET << "          Force ZSTD compression\n";
    std::cout << "  " << Color::WHITE << "--lzma" << Color::RESET << "          Force LZMA compression (default)\n";
    std::cout << "  " << Color::WHITE << "--lz4" << Color::RESET << "           Force LZ4 compression (fast)\n";
    std::cout << "  " << Color::WHITE << "--brotli" << Color::RESET << "        Force Brotli compression\n";
    std::cout << "  " << Color::WHITE << "--store" << Color::RESET << "         No compression (store only)\n";
    
    std::cout << "\n" << Color::BOLD << "Features:" << Color::RESET << "\n";
    std::cout << "  • Solid blocks (1GB) for maximum ratio\n";
    std::cout << "  • Deduplication via XXH64 checksums\n";
    std::cout << "  • Smart codec selection (LZMA/ZSTD/LZ4/Brotli/STORE)\n";
    std::cout << "  • Native ZSTD, LZ4, LZMA, Brotli codec support\n";
    std::cout << "  • Path traversal protection (Zip Slip safe)\n";
    std::cout << "  • xxHash integrity verification on extract\n";
    std::cout << "  • SIMD-accelerated buffer operations (" << SimdOpt::simd_info_string() << ")";
    std::cout << Color::RESET << "\n";
}

void show_help_create() {
    std::cout << Color::BOLD << "Create Archive — " << Color::BRIGHT_WHITE
              << "tarc -c[level] <archive.strk> <file|dir>... [options]"
              << Color::RESET << "\n\n";
    std::cout << Color::BOLD << "Levels:" << Color::RESET << "\n";
    std::cout << "  " << Color::GREEN << "-c1" << Color::RESET << " to " << Color::GREEN << "-c19"
              << Color::RESET << "    Compression level (default: 7)\n";
    std::cout << "  " << Color::GREEN << "-cbest" << Color::RESET << "               Maximum compression\n";
    std::cout << "  " << Color::GREEN << "-cfast" << Color::RESET << "               Fastest compression\n";
    std::cout << "  " << Color::DIM << "Levels 1-9 balance speed/ratio. Levels 10-19 use extreme presets.\n" << Color::RESET;
    
    std::cout << "\n" << Color::BOLD << "Codec Override:" << Color::RESET << "\n";
    std::cout << "  " << Color::WHITE << "--zstd" << Color::RESET << "        ZSTD compression (good for mixed data)\n";
    std::cout << "  " << Color::WHITE << "--lzma" << Color::RESET << "        LZMA2 compression (default, best ratio)\n";
    std::cout << "  " << Color::WHITE << "--lz4" << Color::RESET << "         LZ4/LZ4HC compression (fastest)\n";
    std::cout << "  " << Color::WHITE << "--brotli" << Color::RESET << "      Brotli compression (good for text)\n";
    std::cout << "  " << Color::WHITE << "--store" << Color::RESET << "       No compression (store only)\n";
    std::cout << "  " << Color::DIM << "Without override, TARC auto-selects the best codec per file type.\n" << Color::RESET;
    
    std::cout << "\n" << Color::BOLD << "Options:" << Color::RESET << "\n";
    std::cout << "  " << Color::WHITE << "--threads N" << Color::RESET << "   Parallel compression threads (default: auto)\n";
    std::cout << "  " << Color::WHITE << "--sfx" << Color::RESET << "         Create self-extracting .exe archive\n";
    std::cout << "  " << Color::WHITE << "--no-verify" << Color::RESET << "  Skip xxHash integrity verification\n";
    
    std::cout << "\n" << Color::BOLD << "Examples:" << Color::RESET << "\n";
    std::cout << "  " << Color::DIM << "tarc -c7 backup.strk docs/ images/     Create with default level\n";
    std::cout << "  tarc -c1 --lz4 fast.strk *.txt              Fast compression\n";
    std::cout << "  tarc -cbest --sfx portable.exe src/          Max compression + SFX\n" << Color::RESET;
}

void show_help_extract() {
    std::cout << Color::BOLD << "Extract Files — " << Color::BRIGHT_WHITE
              << "tarc -x <archive.strk> [filter...] [options]"
              << Color::RESET << "\n\n";
    std::cout << Color::BOLD << "Filters:" << Color::RESET << "\n";
    std::cout << "  " << Color::WHITE << "*.txt" << Color::RESET << "          Extract only .txt files\n";
    std::cout << "  " << Color::WHITE << "dir/*" << Color::RESET << "          Extract files from a specific directory\n";
    std::cout << "  " << Color::DIM << "Without filters, all files are extracted.\n" << Color::RESET;
    
    std::cout << "\n" << Color::BOLD << "Options:" << Color::RESET << "\n";
    std::cout << "  " << Color::WHITE << "--output-dir <path>" << Color::RESET << "  Extract to directory\n";
    std::cout << "  " << Color::WHITE << "--flat" << Color::RESET << "          Flatten paths (no subdirectories)\n";
    std::cout << "  " << Color::WHITE << "--force" << Color::RESET << "         Overwrite existing files\n";
    std::cout << "  " << Color::WHITE << "--no-verify" << Color::RESET << "  Skip xxHash integrity check\n";
    
    std::cout << "\n" << Color::BOLD << "Examples:" << Color::RESET << "\n";
    std::cout << "  " << Color::DIM << "tarc -x backup.strk                     Extract all\n";
    std::cout << "  tarc -x backup.strk \"*.cpp\" \"*.h\"         Extract sources only\n";
    std::cout << "  tarc -x backup.strk --output-dir ./restore   Extract to directory\n" << Color::RESET;
}

void show_help_list() {
    std::cout << Color::BOLD << "List Contents — " << Color::BRIGHT_WHITE
              << "tarc -l <archive.strk>"
              << Color::RESET << "\n\n";
    std::cout << "Shows each file in the archive with its codec, size, and compression ratio.\n";
    std::cout << "Duplicate files are marked with (DUPLICATE).\n";
    
    std::cout << "\n" << Color::BOLD << "Example:" << Color::RESET << "\n";
    std::cout << "  " << Color::DIM << "tarc -l backup.strk\n" << Color::RESET;
}

void show_help_test() {
    std::cout << Color::BOLD << "Test Integrity — " << Color::BRIGHT_WHITE
              << "tarc -t <archive.strk>"
              << Color::RESET << "\n\n";
    std::cout << "Reads and decompresses all data, verifying xxHash checksums without\n";
    std::cout << "writing any files to disk.\n";
    
    std::cout << "\n" << Color::BOLD << "Options:" << Color::RESET << "\n";
    std::cout << "  " << Color::WHITE << "--no-verify" << Color::RESET << "  Skip xxHash verification (quick check only)\n";
    
    std::cout << "\n" << Color::BOLD << "Example:" << Color::RESET << "\n";
    std::cout << "  " << Color::DIM << "tarc -t backup.strk\n" << Color::RESET;
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
    auto sec = std::chrono::duration_cast<std::chrono::seconds>(ms).count();
    if (sec >= 60) {
        auto min = sec / 60;
        sec %= 60;
        char buf[32];
        snprintf(buf, sizeof(buf), "%lum %lus", static_cast<unsigned long>(min), static_cast<unsigned long>(sec));
        return std::string(buf);
    }
    return std::to_string(ms.count()) + "ms";
}

void print_info(const std::string& msg) {
    std::cout << Color::CYAN << "[i] " << Color::RESET << msg << "\n";
}

void print_warning(const std::string& msg) {
    safe_print(std::string(Color::YELLOW) + "⚠ " + Color::RESET + msg + "\n");
}

void print_error(const std::string& msg) {
    std::cout << Color::RED << "✖ " << Color::RESET << msg << "\n";
}

void print_success(const std::string& msg) {
    std::cout << Color::GREEN << "✔ " << Color::RESET << msg << "\n";
}

void print_progress(size_t current, size_t total, const std::string& current_file) {
    static std::unique_ptr<ProgressBar> bar;
    static size_t last_total = 0;

    if (!bar || last_total != total) {
        bar = std::make_unique<ProgressBar>(total, "");
        last_total = total;
    }
    bar->update(current, current_file.substr(current_file.find_last_of("/\\") + 1));
}

void print_progress_end() {
    // Handled by ProgressBar destructor
}

void print_add(const std::string& name, uint64_t size, Codec codec, float ratio) {
    bool is_dedup = (ratio >= 1.0f);
    
    std::ostringstream os;
    os << Color::GREEN << "[+]" << Color::RESET << " ["
       << Color::YELLOW << std::setw(5) << codec_name(codec) << Color::RESET << "] "
       << std::left << std::setw(40) << name.substr(0, 40) << " "
       << std::right << std::setw(10) << human_size(size) << "  "
       << Color::DIM << (is_dedup ? "→ DEDUP" : compress_ratio(size, static_cast<uint64_t>(size * (1.0f - ratio))).c_str())
       << Color::RESET;
    
    g_last_status_line = os.str();
    g_has_status_line = true;
}

void print_extract(const std::string& name, uint64_t size, bool test, bool ok) {
    if (!ok) {
        g_last_status_line = std::string(Color::RED) + "[✖] " + Color::RESET + name;
        g_has_status_line = true;
        return;
    }
    std::ostringstream os;
    os << Color::CYAN << "[" << (test ? "OK" : "×") << "]" << Color::RESET << " "
       << std::left << std::setw(42) << name.substr(0, 42) << " "
       << std::right << std::setw(10) << human_size(size);
    g_last_status_line = os.str();
    g_has_status_line = true;
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

void print_summary(const TarcResult& result, const std::string& op, 
                   std::chrono::milliseconds elapsed) {
    std::cout << "\n";
    
    if (!result.ok) {
        // Use cout for guaranteed visibility (stderr may not be visible in some terminals)
        std::cout << Color::RED << "[FAIL] " << op << " failed: " << result.message << Color::RESET << "\n";
        if (result.error != TarcError::None) {
            std::cout << Color::DIM << "  Code: " << error_message(result.error) << Color::RESET << "\n";
        }
        return;
    }
    
    if (result.bytes_in > 0 && result.bytes_out > 0) {
        std::cout << Color::GREEN << "✔ " << op << " completed successfully." << Color::RESET << "\n";
        std::cout << "  " << human_size(result.bytes_in) << " → " << human_size(result.bytes_out) << "  "
                  << Color::DIM << "(" << compress_ratio(result.bytes_in, result.bytes_out) << ")" << Color::RESET << "\n";
        
        // Show speed if we have duration
        if (elapsed.count() > 0) {
            double seconds = elapsed.count() / 1000.0;
            double mbps = (result.bytes_in / (1024.0 * 1024.0)) / seconds;
            std::cout << Color::CYAN << "  Speed: " << Color::BRIGHT_CYAN << std::fixed << std::setprecision(1) 
                      << mbps << Color::CYAN << " MB/s" << Color::RESET << "\n";
        }
        
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
        std::cout << std::left << std::setw(static_cast<int>(widths[i])) << cols[i];
    }
    std::cout << "\n";
}

UI::ProgressBar::ProgressBar(size_t total, const std::string& label)
    : total_(total), current_(0), label_(label), active_(true),
      status_active_(false),
      start_time(TarcUtil::safe_now()), last_update(TarcUtil::safe_now()),
      start_set(false) {
    if (total > 0) update(0);
}

UI::ProgressBar::~ProgressBar() {
    finish();
}

void UI::ProgressBar::set_label(const std::string& label) {
    label_ = label;
}

void UI::ProgressBar::update(size_t current, const std::string& status) {
    current_ = current;
    if (!active_ || total_ == 0) return;

    if (current == 0) return;

    if (!start_set) {
        start_time = TarcUtil::safe_now();
        start_set = true;
    }

    int term_w = get_terminal_width();
    float pct = static_cast<float>(current) / static_cast<float>(total_) * 100.0f;

    char pct_buf[8];
    snprintf(pct_buf, sizeof(pct_buf), "%.1f%%", pct);
    std::string pct_str = pct_buf;

    std::string count_str = std::to_string(current) + "/" + std::to_string(total_) + " files";

    std::string display_name = status;
    const char* bar_color = (current >= total_) ? Color::GREEN : Color::CYAN;

    auto now = TarcUtil::safe_now();
    double elapsed = std::chrono::duration<double>(now - start_time).count();

    std::string suffix;
    if (current < total_ && elapsed > 0.5) {
        double rate = current / elapsed;
        char rate_buf[32];
        snprintf(rate_buf, sizeof(rate_buf), "  %.1f f/s", rate);
        suffix += rate_buf;

        double remaining_sec = (total_ - current) / rate;
        if (remaining_sec >= 1.0) {
            char eta_buf[32];
            if (remaining_sec >= 3600) {
                snprintf(eta_buf, sizeof(eta_buf), "  ETA %d:%02d:%02d",
                         static_cast<int>(remaining_sec / 3600),
                         static_cast<int>(remaining_sec / 60) % 60,
                         static_cast<int>(remaining_sec) % 60);
            } else if (remaining_sec >= 60) {
                snprintf(eta_buf, sizeof(eta_buf), "  ETA %d:%02d",
                         static_cast<int>(remaining_sec / 60),
                         static_cast<int>(remaining_sec) % 60);
            } else {
                snprintf(eta_buf, sizeof(eta_buf), "  ETA %ds",
                         static_cast<int>(remaining_sec));
            }
            suffix += eta_buf;
        }
    }

    std::lock_guard<std::mutex> lock(cout_mutex);

    if (g_has_status_line && !g_last_status_line.empty()) {
        if (!status_active_) {
            // First status line: insert above the progress bar
            std::cout << "\r\x1b[2K" << g_last_status_line << "\n";
            status_active_ = true;
        } else {
            // Subsequent: overwrite status line above, then progress bar
            std::cout << "\x1b[A\r\x1b[2K" << g_last_status_line
                      << "\x1b[B\r\x1b[2K";
        }
        g_has_status_line = false;
    } else {
        std::cout << "\r\x1b[2K";
    }

    std::cout << "  " << bar_color << "[";

    int bar_width = std::clamp(term_w - 55, 10, 40);
    int pos = static_cast<int>(static_cast<size_t>(bar_width) * current / total_);

    for (int i = 0; i < bar_width; ++i) {
        std::cout << (i < pos ? "█" : "░");
    }
    std::cout << "] " << Color::RESET
              << Color::BOLD << pct_str << Color::RESET;

    int remaining = term_w - 5 - bar_width - static_cast<int>(pct_str.size())
                    - static_cast<int>(count_str.size())
                    - static_cast<int>(suffix.size()) - 10;
    if (remaining < 8) remaining = 8;
    std::string name = display_name;
    if (static_cast<int>(name.size()) > remaining) {
        name = name.substr(0, static_cast<size_t>(std::max(0, remaining - 3))) + "...";
    }

    if (!name.empty()) {
        std::cout << "  " << Color::BRIGHT_WHITE << name << Color::RESET;
    }
    std::cout << "  " << Color::DIM << count_str << Color::RESET
              << suffix
              << std::flush;

    last_update = now;
}

void UI::ProgressBar::finish() {
    if (!active_) return;
    active_ = false;
    if (total_ > 0) {
        update(total_);
    }
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

} // namespace UI
