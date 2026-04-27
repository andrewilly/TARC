#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <optional>
#include "types.h"

namespace Color {
    inline const char* RESET  = "\x1b[0m";
    inline const char* BOLD   = "\x1b[1m";
    inline const char* DIM    = "\x1b[2m";
    inline const char* UNDER  = "\x1b[4m";
    
    inline const char* BLACK  = "\x1b[30m";
    inline const char* RED    = "\x1b[31m";
    inline const char* GREEN  = "\x1b[32m";
    inline const char* YELLOW = "\x1b[33m";
    inline const char* BLUE   = "\x1b[34m";
    inline const char* MAGENTA = "\x1b[35m";
    inline const char* CYAN   = "\x1b[36m";
    inline const char* WHITE  = "\x1b[37m";
    
    inline const char* B_BLACK  = "\x1b[40m";
    inline const char* B_RED    = "\x1b[41m";
    inline const char* B_GREEN  = "\x1b[42m";
    inline const char* B_YELLOW = "\x1b[43m";
    inline const char* B_BLUE   = "\x1b[44m";
    inline const char* B_MAGENTA = "\x1b[45m";
    inline const char* B_CYAN   = "\x1b[46m";
    inline const char* B_WHITE  = "\x1b[47m";
    
    inline const char* BRIGHT_BLACK  = "\x1b[90m";
    inline const char* BRIGHT_RED    = "\x1b[91m";
    inline const char* BRIGHT_GREEN  = "\x1b[92m";
    inline const char* BRIGHT_YELLOW = "\x1b[93m";
    inline const char* BRIGHT_BLUE   = "\x1b[94m";
    inline const char* BRIGHT_MAGENTA = "\x1b[95m";
    inline const char* BRIGHT_CYAN   = "\x1b[96m";
    inline const char* BRIGHT_WHITE  = "\x1b[97m";
}

namespace UI {

    void enable_vtp();
    void disable_vtp();
    
    void show_banner();
    void show_help();
    void show_license();
    
    std::string human_size(uint64_t bytes);
    std::string compress_ratio(uint64_t orig, uint64_t comp);
    std::string format_duration(const std::chrono::milliseconds& ms);
    
    void print_info(const std::string& msg);
    void print_warning(const std::string& msg);
    void print_error(const std::string& msg);
    void print_success(const std::string& msg);
    
    void print_progress(size_t current, size_t total, const std::string& current_file);
    void print_progress_end();
    
    void print_add(const std::string& name, uint64_t size, Codec codec, float ratio);
    void print_extract(const std::string& name, uint64_t size, bool test, bool ok);
    void print_delete(const std::string& name);
    void print_list_entry(const std::string& name, uint64_t orig, uint64_t comp, Codec codec);
    void print_summary(const TarcResult& result, const std::string& operation);
    
    void print_spinner(size_t step);
    void print_table_row(const std::vector<std::string>& cols, const std::vector<size_t>& widths);
    
    class ProgressBar {
    public:
        explicit ProgressBar(size_t total, const std::string& label = "");
        ~ProgressBar();
        
        void set_label(const std::string& label);
        void update(size_t current, const std::string& status = "");
        void finish();
        
    private:
        size_t total_;
        size_t current_;
        std::string label_;
        bool active_;
    };
    
    class Spinner {
    public:
        explicit Spinner(const std::string& message);
        ~Spinner();
        
        void spin();
        void finish(bool success = true, const std::string& message = "");
        
    private:
        std::string message_;
        size_t step_;
        bool active_;
    };

}