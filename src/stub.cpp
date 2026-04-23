#include <iostream>
#include <string>
#include <vector>
#include <windows.h>
#include <fstream>
#include <cstdint>   
#include <cstring>   
#include "engine.h"
#include "ui.h"

// Disabilita un warning di MSVC dovuto all'uso di __try con oggetti C++ (come std::string)
#pragma warning(disable: 4530)

#define TARC_MAGIC_STR "TRC2"

void show_stub_header() {
    std::cout << Color::CYAN << "==========================================\n";
    std::cout << "          TARC SELF-EXTRACTOR             \n";
    std::cout << "       Powered by Strike Engine v2.0      \n";
    std::cout << "==========================================\n" << Color::RESET;
}

size_t find_archive_offset(const std::string& exe_path) {
    std::ifstream f(exe_path, std::ios::binary | std::ios::ate);
    if (!f) return 0;

    std::streamsize file_size = f.tellg();
    if (file_size < 8) return 0;

    uint64_t archive_offset = 0;
    f.seekg(-8, std::ios::end);
    f.read(reinterpret_cast<char*>(&archive_offset), 8);

    if (archive_offset > 0 && archive_offset < (static_cast<uint64_t>(file_size) - 8)) {
        char magic[4] = {0};
        f.seekg(archive_offset, std::ios::beg);
        f.read(magic, 4);
        
        if (memcmp(magic, TARC_MAGIC_STR, 4) == 0) {
            return static_cast<size_t>(archive_offset);
        }
    }
    return 0;
}

int main(int argc, char* argv[]) {
    UI::enable_vtp();
    show_stub_header();

    char path[MAX_PATH];
    if (GetModuleFileNameA(NULL, path, MAX_PATH) == 0) {
        UI::print_error("Errore: Impossibile determinare il percorso del file.");
        std::cout << "Premere INVIO per uscire...";
        std::cin.get();
        return 1;
    }
    std::string self_path = path;

    size_t offset = find_archive_offset(self_path);

    if (offset == 0) {
        UI::print_error("Errore: Dati TRC2 non trovati. Questo non e' un archivio SFX valido.");
        std::cout << "Premere INVIO per uscire...";
        std::cin.get();
        return 1;
    }

    std::cout << "\n" << Color::YELLOW << " OPERAZIONI DISPONIBILI:" << Color::RESET << "\n";
    std::cout << " [1] Estrai tutto (cartella corrente)\n";
    std::cout << " [2] Elenca files\n";
    std::cout << " [3] Verifica integrita' (Checksum)\n";
    std::cout << " [4] Esci\n";
    std::cout << "\nScelta > ";

    std::string input;
    std::getline(std::cin, input);
    if (input.empty()) return 0;
    char choice = input[0];

    TarcResult res = {false, ""};
    bool has_crashed = false;

    // === GABBIA ANTI-CRASH DI WINDOWS ===
    // Intercetta gli Access Violation prima che Windows chiuda il programma
    __try {
        switch (choice) {
            case '1':
                UI::print_info("Estrazione in corso...");
                res = Engine::extract(self_path, {}, false, offset);
                UI::print_summary(res, "Estrazione SFX");
                break;
            case '2':
                UI::print_info("Contenuto dell'archivio:");
                res = Engine::list(self_path, offset);
                break;
            case '3':
                UI::print_info("Verifica integrita' in corso...");
                res = Engine::extract(self_path, {}, true, offset);
                UI::print_summary(res, "Test SFX");
                break;
            case '4':
                return 0;
            default:
                UI::print_warning("Scelta non valida.");
                break;
        }

        if (!res.ok && choice != '4') {
            UI::print_error("Operazione fallita:");
            UI::print_error("Dettaglio: " + res.message);
        }
    } 
    __except(EXCEPTION_EXECUTE_HANDLER) {
        UI::print_error("ACCESS VIOLATION: Il programma e' crashato durante la decompressione!");
        UI::print_error("Probabile causa: Dati corrotti o strutturale Header incompatibile.");
        has_crashed = true;
    }
    // =====================================

    std::cout << "\nProcedura terminata. Premere INVIO per uscire...";
    std::cin.get();

    return has_crashed ? 1 : (res.ok ? 0 : 1);
}
