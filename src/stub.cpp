#include <iostream>
#include <string>
#include <vector>
#include <windows.h>
#include <fstream>
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

// Cerca l'offset dell'archivio partendo dal fondo del file
size_t find_archive_offset(const std::string& exe_path) {
    std::ifstream f(exe_path, std::ios::binary | std::ios::ate);
    if (!f) return 0;

    size_t file_size = static_cast<size_t>(f.tellg());
    if (file_size < 4) return 0;

    // Carichiamo il file in memoria per una scansione rapida
    std::vector<char> buffer(file_size);
    f.seekg(0, std::ios::beg);
    f.read(buffer.data(), file_size);

    // Cerchiamo la firma "TRC2" partendo dal fondo verso l'inizio.
    // Saltiamo i primi 100KB per evitare di trovare stringhe nel codice dello stub.
    for (size_t i = file_size - 4; i > 1024 * 100; --i) {
        if (buffer[i] == 'T' && buffer[i+1] == 'R' && buffer[i+2] == 'C' && buffer[i+3] == '2') {
            return i;
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
        return 1;
    }
    std::string self_path = path;

    // 2. Trova l'offset dell'archivio appeso
    size_t offset = find_archive_offset(self_path);

    // --- DEBUG INFO ---
    std::ifstream f_sz(self_path, std::ios::binary | std::ios::ate);
    size_t total_size = f_sz.tellg();
    
    std::cout << Color::YELLOW << "[DEBUG] Dimensione EXE totale: " << total_size << " bytes\n";
    std::cout << "[DEBUG] Offset archivio trovato: " << offset << " bytes\n";
    if (offset > 0) {
        std::cout << "[DEBUG] Dimensione dati rilevati: " << (total_size - offset) << " bytes\n" << Color::RESET;
    }
    // ------------------

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
            return 0;
    }

    // 4. Gestione errori se l'operazione fallisce
    if (!res.ok) {
        UI::print_error("L'operazione ha riscontrato un errore:");
        UI::print_error("Dettaglio: " + res.message);
    }

    std::cout << "\nProcedura terminata. Premere INVIO per uscire...";
    std::cin.get();

    return res.ok ? 0 : 1;
}
