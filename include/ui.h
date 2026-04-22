#pragma once
#include <string>
#include <cstdint>
#include "types.h"

namespace Color {
    inline constexpr const char* RESET  = "\x1b[0m";
    inline constexpr const char* BOLD   = "\x1b[1m";
    inline constexpr const char* CYAN   = "\x1b[38;5;81m";
    inline constexpr const char* GREEN  = "\x1b[32m";
    inline constexpr const char* YELLOW = "\x1b[33m";
    inline constexpr const char* RED    = "\x1b[31m";
    inline constexpr const char* WHITE  = "\x1b[37m";
    inline constexpr const char* DIM    = "\x1b[2m";
}

namespace UI {
    void enable_vtp();
    void show_help();
    void show_banner();
    
    [[nodiscard]] std::string human_size(uint64_t bytes);
    [[nodiscard]] std::string compress_ratio(uint64_t orig, uint64_t comp);
    
    void print_add(const std::string& name, uint64_t size, Codec codec, float ratio);
    void print_extract(const std::string& name, uint64_t size, bool test, bool ok);
    void print_delete(const std::string& name);
    void print_list_entry(const std::string& name, uint64_t orig, uint64_t comp, Codec codec);
    void print_summary(const TarcResult& result, const std::string& operation);
    
    void print_info(const std::string& msg);
    void print_error(const std::string& msg);
    void print_warning(const std::string& msg);
    void print_progress(size_t current, size_t total, const std::string& current_file);
    
    void print_verbose(const std::string& msg);
    void set_verbose(bool enabled);
}
