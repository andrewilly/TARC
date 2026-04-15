#include <iostream>
#include <string>
#include <vector>
#include <windows.h>
#include <fstream>
#include "engine.h"
#include "ui.h"

// Funzione per mostrare l'intestazione professionale
void show_stub_header() {
    std::cout << Color::CYAN << "==========================================\n";
    std::cout << "          TARC SELF-EXTRACTOR             \n";
    std::cout << "       Powered by Strike Engine v2.0      \n";
    std::cout << "==========================================\n" << Color::RESET;
}

// Trova l'offset dei dati TARC nel file EXE partendo dalla fine
size_t find_archive_offset(const std::string& exe_path) {
    std::ifstream f(exe_path, std::ios::binary | std::ios::ate);
    if (!f) return 0;

    size_t file_size = static_cast<size_t>(f.tellg());
    if (file_size < 4) return 0;

    // Leggiamo il file a ritroso a blocchi di 64KB per efficienza
    const size_t chunk_size = 65536;
    std::vector<char> buffer(chunk_size);
    
    size_t current_pos = file_size;
    while (current_pos > 0) {
        size_t to_read = (current_pos > chunk_size) ? chunk_size : current_pos;
        current_pos -= to_read;
        
        f.seekg(current_pos, std::ios::beg);
        f.read(buffer.data(), to_read);
        
        // Cerchiamo la firma "TARC" nel blocco (dalla fine all'inizio)
        for (long i = static_cast<long>(to_read) - 4; i >= 0; --i) {
            if (buffer[i] == 'T' && buffer[i+1] == 'A' && buffer[i+2] == 'R' && buffer[i+3] == 'C') {
                return current_pos + i;
            }
        }
        
        // Se non siamo all'inizio, sovrapponiamo di 3 byte per non perdere il magic tra blocchi
        if (current_pos > 0) current_pos += 3; 
    }
    return 0;
}

int main(int argc, char* argv[]) {
    // 1. Inizializzazione Ambiente Windows
    UI::enable_vtp();
    show_stub_header();

    // 2. Recupero percorso eseguibile
    char path[MAX_PATH];
    if (GetModuleFileNameA(NULL, path, MAX_PATH) == 0) {
        UI::print_error("Errore fatale: Impossibile mappare il processo.");
        return 1;
    }
    std::string self_path = path;

    // 3. Individuazione Archivio
    size_t offset = find_archive_offset(self_path);
    if (offset == 0) {
        UI::print_error("Errore: Dati dell'archivio non trovati in questo eseguibile.");
        std::cout << "\nPremere INVIO per uscire...";
        std::cin.get();
        return 1;
    }

    // 4. Menu Interattivo
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
    
    // 5. Esecuzione comando
    switch (choice) {
        case '1':
            UI::print_info("Avvio estrazione solida...");
            res = Engine::extract(self_path, {}, false, offset);
            UI::print_summary(res, "Estrazione SFX");
            break;
        case '2':
            UI::print_info("Contenuto dell'archivio:");
            res = Engine::list(self_path, offset);
            break;
        case '3':
            UI::print_info("Test dei blocchi in corso...");
            res = Engine::extract(self_path, {}, true, offset);
            UI::print_summary(res, "Test SFX");
            break;
        case '4':
            return 0;
        default:
            UI::print_warning("Scelta non valida.");
            return 0;
    }

    // 6. Gestione Errori Motore
    if (!res.ok) {
        UI::print_error("ERRORE DURANTE L'OPERAZIONE:");
        UI::print_error("-> " + res.message);
    }

    std::cout << "\nProcedura terminata. Premere INVIO per chiudere...";
    std::cin.get();

    return res.ok ? 0 : 1;
}
