#include <iostream>
#include <string>
#include <vector>
#include <windows.h>
#include "engine.h"
#include "ui.h"

// --- HEADER DELLO STUB ---
void show_stub_header() {
    std::cout << Color::CYAN << "==========================================\n";
    std::cout << "          TARC SELF-EXTRACTOR             \n";
    std::cout << "       Powered by Strike Engine v2.0      \n";
    std::cout << "==========================================\n" << Color::RESET;
}

// Funzione per individuare l'inizio dei dati TARC dentro l'eseguibile stesso
size_t find_archive_offset(const std::string& exe_path) {
    FILE* f = fopen(exe_path.c_str(), "rb");
    if (!f) return 0;

    char buf[8192];
    size_t total_read = 0;
    
    while (size_t rd = fread(buf, 1, sizeof(buf), f)) {
        for (size_t i = 0; i < rd - 4; ++i) {
            if (memcmp(buf + i, "TARC", 4) == 0) {
                size_t final_offset = total_read + i;
                fclose(f);
                return final_offset;
            }
        }
        total_read += rd;
        if (total_read > 10 * 1024 * 1024) break; // Limite sicurezza 10MB
    }
    fclose(f);
    return 0;
}

void show_stub_menu() {
    std::cout << "\n" << Color::YELLOW << " SELEZIONA OPERAZIONE SUI DATI:" << Color::RESET << "\n";
    std::cout << " [1] Estrai tutto qui\n";
    std::cout << " [2] Elenca files contenuti\n";
    std::cout << " [3] Verifica integrita' (Checksum)\n";
    std::cout << " [4] Esci\n";
    std::cout << "\n Scelta > ";
}

int main(int argc, char* argv[]) {
    // Inizializzazione colori e console
    UI::enable_vtp();
    show_stub_header();

    // Ottieni il percorso di se stesso (l'EXE corrente)
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    std::string self_path = path;

    // Localizza i dati dell'archivio
    size_t offset = find_archive_offset(self_path);
    if (offset == 0) {
        UI::print_error("Errore: Questo eseguibile non contiene dati TARC validi.");
        std::cout << "Premere un tasto per chiudere...";
        std::cin.get();
        return 1;
    }

    show_stub_menu();
    
    char choice;
    std::cin >> choice;
    std::cin.ignore(); 

    TarcResult res;
    switch (choice) {
        case '1':
            UI::print_info("Estrazione in corso...");
            // Passiamo l'offset ai motori di estrazione
            res = Engine::extract(self_path, {}, false, offset);
            UI::print_summary(res, "Estrazione SFX");
            break;
        
        case '2':
            UI::print_info("Lista file nell'archivio:");
            res = Engine::list(self_path, offset);
            break;

        case '3':
            UI::print_info("Verifica integrita' (XXH64)...");
            res = Engine::extract(self_path, {}, true, offset);
            UI::print_summary(res, "Test SFX");
            break;

        case '4':
            return 0;

        default:
            UI::print_warning("Scelta non valida.");
            break;
    }

    std::cout << "\nOperazione completata. Premere INVIO per uscire...";
    std::cin.get();

    return res.ok ? 0 : 1;
}
