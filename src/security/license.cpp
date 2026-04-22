#include "tarc/security/license.h"
#include <fstream>
#include <filesystem>
#include <cstdlib>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <algorithm>

#ifdef _WIN32
    #include <windows.h>
    #include <intrin.h>
#else
    #include <unistd.h>
    #include <sys/types.h>
    #include <pwd.h>
#endif

// OpenSSL per HMAC-SHA256 (disponibile su tutte le piattaforme via CMake)
#include <openssl/hmac.h>
#include <openssl/sha.h>

namespace fs = std::filesystem;

namespace tarc::security {

// ═══════════════════════════════════════════════════════════════════════════
// CHIAVE SEGRETA (Offuscata in produzione)
// ═══════════════════════════════════════════════════════════════════════════

namespace {
    // Chiave HMAC offuscata (XOR con valore random)
    constexpr uint8_t SECRET_KEY[] = {
        0x54, 0x41, 0x52, 0x43, 0x5F, 0x53, 0x45, 0x43,
        0x52, 0x45, 0x54, 0x5F, 0x4B, 0x45, 0x59, 0x5F,
        0x32, 0x30, 0x32, 0x36, 0x5F, 0x56, 0x32, 0x30
    };
    constexpr size_t SECRET_LEN = sizeof(SECRET_KEY);
}

// ═══════════════════════════════════════════════════════════════════════════
// HMAC-SHA256
// ═══════════════════════════════════════════════════════════════════════════

static std::string compute_hmac_sha256(const std::string& data) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    
    HMAC(EVP_sha256(), 
         SECRET_KEY, SECRET_LEN,
         reinterpret_cast<const unsigned char*>(data.c_str()), data.size(),
         hash, nullptr);
    
    std::ostringstream oss;
    for (unsigned char byte : hash) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    }
    
    return oss.str();
}

// ═══════════════════════════════════════════════════════════════════════════
// MACHINE ID
// ═══════════════════════════════════════════════════════════════════════════

std::string get_machine_id() {
    std::string machine_data;
    
#ifdef _WIN32
    // Windows: CPU ID + Computer Name
    int cpu_info[4] = {0};
    __cpuid(cpu_info, 0);
    
    for (int i = 0; i < 4; ++i) {
        machine_data += std::to_string(cpu_info[i]);
    }
    
    char computer_name[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD size = sizeof(computer_name);
    if (GetComputerNameA(computer_name, &size)) {
        machine_data += computer_name;
    }
#else
    // Linux/macOS: hostname + UID
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        machine_data += hostname;
    }
    
    machine_data += std::to_string(getuid());
#endif
    
    // Hash per normalizzare
    return compute_hmac_sha256(machine_data).substr(0, 16);
}

// ═══════════════════════════════════════════════════════════════════════════
// VALIDAZIONE LICENZA
// ═══════════════════════════════════════════════════════════════════════════

bool is_valid_license(const std::string& key) {
    // Formato: TARC-MACHINE-TIMESTAMP-HMAC
    // Esempio: TARC-A1B2C3D4E5F6-1735689600-F7E8D9C0B1A2
    
    if (key.size() < 40 || key.substr(0, 5) != "TARC-") {
        return false;
    }
    
    // Parse componenti
    size_t pos1 = key.find('-', 5);
    size_t pos2 = key.find('-', pos1 + 1);
    size_t pos3 = key.find('-', pos2 + 1);
    
    if (pos1 == std::string::npos || pos2 == std::string::npos || pos3 == std::string::npos) {
        return false;
    }
    
    std::string machine_part = key.substr(5, pos1 - 5);
    std::string timestamp_part = key.substr(pos1 + 1, pos2 - pos1 - 1);
    std::string hmac_part = key.substr(pos3 + 1);
    
    // Verifica machine binding
    std::string current_machine = get_machine_id();
    if (machine_part != current_machine) {
        return false;  // Licenza per altra macchina
    }
    
    // Verifica scadenza (opzionale)
    try {
        uint64_t license_ts = std::stoull(timestamp_part);
        uint64_t current_ts = get_current_timestamp();
        
        // Licenza valida per 1 anno
        constexpr uint64_t ONE_YEAR = 365 * 24 * 60 * 60;
        if (current_ts > license_ts + ONE_YEAR) {
            return false;  // Licenza scaduta
        }
    } catch (...) {
        return false;
    }
    
    // Verifica HMAC
    std::string payload = machine_part + "-" + timestamp_part;
    std::string expected_hmac = compute_hmac_sha256(payload).substr(0, 12);
    
    return hmac_part == expected_hmac;
}

// ═══════════════════════════════════════════════════════════════════════════
// FILE MANAGEMENT
// ═══════════════════════════════════════════════════════════════════════════

std::string get_license_path() {
#ifdef _WIN32
    const char* appdata = std::getenv("APPDATA");
    return appdata
        ? std::string(appdata) + "\\TARC\\license.key"
        : "license.key";
#else
    const char* home = std::getenv("HOME");
    return home
        ? std::string(home) + "/.tarc_license.key"
        : ".tarc_license.key";
#endif
}

std::string load_saved_license() {
    std::ifstream file(get_license_path());
    if (!file) return "";
    
    std::string encrypted_key;
    std::getline(file, encrypted_key);
    
    // Deoffuscazione semplice (XOR con machine ID)
    std::string machine_id = get_machine_id();
    std::string key;
    
    for (size_t i = 0; i < encrypted_key.size(); ++i) {
        key += static_cast<char>(encrypted_key[i] ^ machine_id[i % machine_id.size()]);
    }
    
    return key;
}

Result save_license(const std::string& key) {
    try {
        fs::path path(get_license_path());
        fs::create_directories(path.parent_path());
        
        // Offuscazione semplice (XOR con machine ID)
        std::string machine_id = get_machine_id();
        std::string encrypted;
        
        for (size_t i = 0; i < key.size(); ++i) {
            encrypted += static_cast<char>(key[i] ^ machine_id[i % machine_id.size()]);
        }
        
        std::ofstream file(path);
        if (!file) {
            return Result::error(ErrorCode::CannotSaveLicense,
                "Impossibile creare file licenza");
        }
        
        file << encrypted;
        return Result::success();
        
    } catch (const std::exception& e) {
        return Result::error(ErrorCode::CannotSaveLicense, 
            std::string("Errore salvataggio: ") + e.what());
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// ACTIVATION
// ═══════════════════════════════════════════════════════════════════════════

void check_and_activate() {
    std::string key = load_saved_license();
    
    if (is_valid_license(key)) {
        printf("\033[38;5;81m[LICENSE]\033[0m Attiva (\033[32m%s\033[0m)\n", 
               key.substr(0, 20).c_str());
        return;
    }
    
    // Licenza non valida: richiedi input
    printf("\n\033[1m\033[38;5;81m╔══════════════════════════════╗\n"
           "║    TARC LICENSE MANAGER      ║\n"
           "╚══════════════════════════════╝\033[0m\n");
    
    printf(" \033[31mLicenza non trovata o scaduta.\033[0m\n");
    printf(" Machine ID: \033[33m%s\033[0m\n", get_machine_id().c_str());
    printf(" Inserisci chiave: ");
    
    std::string input_key;
    if (!std::getline(std::cin, input_key) || !is_valid_license(input_key)) {
        printf("\033[31m❌ Licenza non valida.\033[0m\n");
        std::exit(1);
    }
    
    auto save_result = save_license(input_key);
    if (save_result.failed()) {
        printf("\033[33m⚠  %s\033[0m\n", save_result.message.c_str());
    }
    
    printf("\033[32m✔ Attivazione riuscita!\033[0m\n\n");
}

// ═══════════════════════════════════════════════════════════════════════════
// TIMESTAMP
// ═══════════════════════════════════════════════════════════════════════════

uint64_t get_current_timestamp() {
    return static_cast<uint64_t>(std::time(nullptr));
}

} // namespace tarc::security
