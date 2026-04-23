#pragma once
#include <string>

namespace License {
    bool is_valid(const std::string& key);
    std::string get_license_path();
    std::string load_saved_key();
    bool save_key(const std::string& key);
    void check_and_activate();
}