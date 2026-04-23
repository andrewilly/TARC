#pragma once
#include <string>
#include <vector>
#include "types.h"

namespace Color {
    extern const char* RED;
    extern const char* GREEN;
    extern const char* YELLOW;
    extern const char* BLUE;
    extern const char* MAGENTA;
    extern const char* CYAN;
    extern const char* WHITE;
    extern const char* RESET;
    extern const char* BOLD;
    extern const char* DIM;
}

namespace UI {
    extern bool g_verbose;

    void enable_vtp();
    void show_banner();
    void show_help();

    void print_info(const std::string& msg);
    void print_success(const std::string& msg);
    void print_warning(const std::string& msg);
    void print_error(const std::string& msg);
    void print_verbose(const std::string& msg);

    void progress_timer_reset();
    void update_progress(uint64_t current, uint64_t total, const std::string& label);

    void print_summary(const TarcResult& r, const std::string& title);
    std::string human_size(uint64_t bytes);
}