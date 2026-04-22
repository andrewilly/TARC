#pragma once
#include <string>
#include "tarc/util/result.h"
#include "tarc/util/types.h"

namespace tarc::ui {

// ═══════════════════════════════════════════════════════════════════════════
// INITIALIZATION
// ═══════════════════════════════════════════════════════════════════════════

void enable_vtp();
void show_banner();
void show_help();

// ═══════════════════════════════════════════════════════════════════════════
// FORMATTING
// ═══════════════════════════════════════════════════════════════════════════

[[nodiscard]] std::string human_size(uint64_t bytes);
[[nodiscard]] std::string compress_ratio(uint64_t orig, uint64_t comp);

// ═══════════════════════════════════════════════════════════════════════════
// OUTPUT
// ═══════════════════════════════════════════════════════════════════════════

void print_info(const std::string& msg);
void print_error(const std::string& msg);
void print_warning(const std::string& msg);
void print_summary(const Result& result, const std::string& operation);
void print_progress(size_t current, size_t total, const std::string& filename);

} // namespace tarc::ui
