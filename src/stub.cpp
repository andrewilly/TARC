#include <iostream>
#include <string>
#include <vector>
#include <windows.h>
#include <fstream>
#include <cstdint>   
#include <cstring>   
#include <exception> // Aggiunto per std::exception
#include "engine.h"
#include "ui.h"

// Usiamo la definizione trovata in types.h
#define TARC_MAGIC_STR "TRC2"

void show_stub_header() {
    std::cout << Color::CYAN << "==========================================\n";
    std::cout << "          TARC SELF-EXTRACTOR             \n";
    std::cout << "       Powered by Strike Engine v2.0      \n";
    std::cout << "==========================================\n" << Color::RESET;
}

// Nuovo metodo: Legge l'offset esatto dagli ultimi 8 byte del file (Footer)
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

    // 1. Recupera il percorso dell'eseguibile corrente
    char path[MAX_PATH];
    if (GetModuleFileNameA(NULL, path, MAX_PATH) == 0) {
        UI::print_error("Errore: Impossibile determinare il percorso del file.");
        std::cout << "Premere INVIO per uscire...";
        std::cin.get();
        return 1;
    }
    std::string self_path = path;

    // 2. Trova l'offset dell'archivio appeso tramite il Footer
    size_t offset = find_archive_offset(self_path);

    if (offset == 0) {
        UI::print_error("Errore: Dati TRC2 non trovati. Questo non e' un archivio SFX valido.");
        std::cout << "Premere INVIO per uscire...";
        std::cin.get();
        return 1;
    }

    // 3. Menu Operativo
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

    TarcResult res;
    bool has_crashed = false;

    // === BLOCCO TRY-CATCH ANTI-CRASH ===
    try {
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

        // Gestione errori logici (senza crash)
        if (!res.ok && choice != '4' && choice != '2') {
            UI::print_error("L'operazione ha riscontrato un errore:");
            UI::print_error("Dettaglio: " + res.message);
        }
    } 
    catch (const std::exception& e) {
        // Intercetta errori di memoria (es. bad_alloc) o eccezioni C++
        UI::print_error("CRASH IMPREVISTO durante l'operazione!");
        UI::print_error(std::string("Tipo errore: ") + e.what());
        has_crashed = true;
    }
    catch (...) {
        // Intercetta tutti gli altri crash gravi (es. Access Violation)
        UI::print_error("CRASH GRAVE: Access Violation o errore di sistema sconosciuto.");
        has_crashed = true;
    }
    // ==================================

    std::cout << "\nProcedura terminata. Premere INVIO per uscire...";
    std::cin.get();

    return has_crashed ? 1 : (res.ok ? 0 : 1);
}
