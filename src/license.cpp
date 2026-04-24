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
    #include <bcrypt.h>
#endif

namespace fs = std::filesystem;

namespace {
    LicenseInfo g_license_info;
}

std::optional<LicenseInfo> License::load_saved_key() {
    std::string path = get_license_path();
    if (!fs::exists(path)) return std::nullopt;
    
    try {
        std::ifstream ifs(path);
        if (!ifs) return std::nullopt;
        
        LicenseInfo info;
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
        
        LicenseInfo info;
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

bool License::save_license_info(const LicenseInfo& info) {
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
            g_license_info = {};
            return true;
        } catch (...) {
            return false;
        }
    }
    return true;
}

void License::set_license_info(const LicenseInfo& info) {
    g_license_info = info;
}

LicenseInfo License::get_license_info() {
    return g_license_info;
}

void License::check_and_activate() {
    auto saved = load_saved_key();
    
    if (saved && saved->is_valid) {
        std::cout << Color::CYAN << "[" << Color::GREEN << "LICENSE" << Color::CYAN << "] " << Color::RESET;
        std::cout << "Active (" << Color::BRIGHT_GREEN << saved->key << Color::RESET << ")\n";
        return;
    }

    std::cout << Color::BOLD << Color::CYAN << "\n╔════════════════════════════════════════╗\n";
    std::cout << "║     TARC LICENSE MANAGER        ║\n";
    std::cout << "╚════════════════════════════════════════╝" << Color::RESET << "\n\n";
    
    std::cout << Color::YELLOW << "⚠ " << Color::RESET << "License not found or invalid.\n";
    std::cout << "Enter license key (or press Enter for trial): ";
    
    std::string key;
    if (!std::getline(std::cin, key)) {
        UI::print_error("No input received. Operation cancelled.");
        std::exit(1);
    }
    
    if (key.empty()) {
        key = generate_trial_key();
        if (save_license_info({key, "Trial User", "", true})) {
            std::cout << Color::GREEN << "✔ " << Color::RESET << "Trial activated: " << key << "\n";
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
    
    std::cout << Color::GREEN << "✔ " << Color::RESET << "Activation successful!\n\n";
}