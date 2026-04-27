#pragma once
#include <string>
#include <optional>
#include <chrono>
#include "types.h"

namespace License {

    struct LicenseInfo {
        std::string key;
        std::string user;
        std::string expiry;
        bool is_valid = false;
        bool is_trial = false;
    };

    bool is_valid(const std::string& key);
    bool is_valid_key_format(const std::string& key);
    
    std::string get_license_path();
    std::string get_config_path();
    
    std::optional<LicenseInfo> load_saved_key();
    bool save_key(const std::string& key);
    bool save_license_info(const LicenseInfo& info);
    
    bool delete_license();
    
    void check_and_activate(bool show_full_info = true);
    
    std::string generate_trial_key();
    std::string hash_key(const std::string& key);
    
    void set_license_info(const LicenseInfo& info);
    LicenseInfo get_license_info();

}