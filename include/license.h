#pragma once
#include <string>

namespace License {
    [[nodiscard]] bool is_valid(const std::string& key);
    [[nodiscard]] std::string get_license_path();
    [[nodiscard]] std::string load_saved_key();
    [[nodiscard]] bool save_key(const std::string& key);
    void check_and_activate();
}
