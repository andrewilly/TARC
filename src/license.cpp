#include "license.h"
#include "ui.h"
#include "types.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <cstdlib>
#include <ctime>
#include <random>
#include <iostream>

#ifdef _WIN32
    #include <windows.h>
    #include <wincrypt.h>
    #include <bcrypt.h>
#endif

extern "C" {
    #include "xxhash.h"
}

namespace fs = std::filesystem;

namespace {
    License::LicenseInfo g_license_info;
}

std::optional<License::LicenseInfo> License::load_saved_key() {
    std::string path = get_license_path();
    if (!fs::exists(path)) return std::nullopt;
    
    try {
        std::ifstream ifs(path);
        if (!ifs) return std::nullopt;
        
        License::LicenseInfo info;
        std::string line;
        
        while (std::getline(ifs, line)) {
            auto eq_pos = line.find('=');
            if (eq_pos == std::string::npos) continue;
            
            std::string key = line.substr(0, eq_pos);
            std::string value = line.substr(eq_pos + 1);
            
            if (key == "key") info.key = value;
            else if (key == "user") info.user = value;
            else if (key == "expiry") info.expiry = value;
            else if (key == "trial") info.is_trial = (value == "1");
        }
        
        if (!info.key.empty() && is_valid(info.key)) {
            info.is_valid = true;
            g_license_info = info;
            return info;
        }
    } catch (...) {
    }
    
    return std::nullopt;
}

bool License::save_key(const std::string& key) {
    try {
        fs::path p(get_license_path());
        fs::create_directories(p.parent_path());
        
        std::ofstream ofs(p);
        if (!ofs) return false;
        
        License::LicenseInfo info;
        info.key = key;
        info.is_valid = is_valid(key);
        
        ofs << "key=" << key << "\n";
        ofs << "user=default\n";
        ofs << "trial=0\n";
        ofs.close();
        
        g_license_info = info;
        return true;
    } catch (...) {
        return false;
    }
}

bool License::save_license_info(const License::LicenseInfo& info) {
    try {
        fs::path p(get_license_path());
        fs::create_directories(p.parent_path());
        
        std::ofstream ofs(p);
        if (!ofs) return false;
        
        ofs << "key=" << info.key << "\n";
        ofs << "user=" << info.user << "\n";
        ofs << "expiry=" << info.expiry << "\n";
        ofs << "trial=" << (info.is_trial ? "1" : "0") << "\n";
        ofs.close();
        
        g_license_info = info;
        return true;
    } catch (...) {
        return false;
    }
}

bool License::delete_license() {
    std::string path = get_license_path();
    if (fs::exists(path)) {
        try {
            fs::remove(path);
            g_license_info = License::LicenseInfo{};
            return true;
        } catch (...) {
            return false;
        }
    }
    return true;
}

void License::set_license_info(const License::LicenseInfo& info) {
    g_license_info = info;
}

License::LicenseInfo License::get_license_info() {
    return g_license_info;
}

void License::check_and_activate(bool show_full_info) {
    auto saved = load_saved_key();
    
    if (saved && saved->is_valid) {
        std::cout << Color::CYAN << "[LICENSE] " << Color::RESET;
        std::cout << "Activated" << Color::DIM << " (" << saved->key << ")" << Color::RESET << "\n";
        return;
    }

    if (show_full_info) {
        std::cout << Color::CYAN << "\n";
        std::cout << "  TARC LICENSE MANAGER\n";
        std::cout << "  --------------------\n\n";
        
        std::cout << Color::YELLOW << "  License not found or invalid.\n";
        std::cout << "  Enter license key (or press Enter for trial): ";
        
        std::string key;
        if (!std::getline(std::cin, key)) {
            UI::print_error("No input received. Operation cancelled.");
            std::exit(1);
        }
        
        if (key.empty()) {
            key = generate_trial_key();
            if (save_license_info({key, "Trial User", "", true})) {
                std::cout << Color::GREEN << "  Trial activated: " << key << Color::RESET << "\n\n";
                return;
            }
        }
        
        if (!is_valid(key)) {
            UI::print_error("Invalid key format.");
            std::exit(1);
        }
        
        if (!save_key(key)) {
            UI::print_warning("Could not save license to disk.");
        }
        
        std::cout << Color::GREEN << "  Activation successful!" << Color::RESET << "\n\n";
    }
}

bool License::is_valid(const std::string& key) {
    if (key.empty() || key.length() < 16) return false;
    
    std::string hash = hash_key(key);
    return !hash.empty() && hash.length() >= 16;
}

bool License::is_valid_key_format(const std::string& key) {
    if (key.empty()) return false;
    
    for (char c : key) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') {
            return false;
        }
    }
    
    return key.length() >= 16 && key.length() <= 64;
}

std::string License::get_license_path() {
    std::string config_dir;
#ifdef _WIN32
    char* appdata = std::getenv("APPDATA");
    config_dir = appdata ? std::string(appdata) : ".";
    config_dir += "\\TARC";
#elif defined(__APPLE__)
    char* home = std::getenv("HOME");
    config_dir = home ? std::string(home) + "/Library/Application Support" : ".";
    config_dir += "/TARC";
#else
    char* home = std::getenv("HOME");
    config_dir = home ? std::string(home) + "/.config" : ".";
    config_dir += "/tarc";
#endif
    return config_dir + "/license.dat";
}

std::string License::get_config_path() {
    return get_license_path();
}

std::string License::generate_trial_key() {
    const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::string key;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 35);
    
    for (int i = 0; i < 24; i++) {
        if (i > 0 && i % 4 == 0) key += '-';
        key += chars[dis(gen)];
    }
    
    return "TRIAL-" + key;
}

std::string License::hash_key(const std::string& key) {
    // SEC-008: Usa xxHash64 invece di std::hash (debole e reversibile)
    // xxHash e' velocissimo e distribuito uniformemente, disponibile su tutte le piattaforme
    std::string salted = key + "TARC_SALT_2024_STRIKE";
    uint64_t h1 = XXH64(salted.c_str(), salted.size(), 0x5A17C1E7B4F2D890ULL);
    uint64_t h2 = XXH64(salted.c_str(), salted.size(), h1);

    // Combina i due hash per produrre una stringa esadecimale
    char buf[33];
    snprintf(buf, sizeof(buf), "%016llx%016llx",
             static_cast<unsigned long long>(h1),
             static_cast<unsigned long long>(h2));
    return std::string(buf, 32);
}