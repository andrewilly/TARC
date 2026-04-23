#include <iostream>
#include <string>
#include <vector>
#include <windows.h>
#include <fstream>
#include <cstdint>   
#include <cstring>   
#include "engine.h"
#include "ui.h"

#define TARC_MAGIC_STR "TRC2"

// === VARIABILI GLOBALI DI SUPPORTO ===
bool g_ok = false;
std::string g_message = "";
bool g_crashed = false;

// === FUNZIONI SCUDO (WRAPPER) ===
// Queste funzioni non contengono __try, quindi possiamo usare std::string liberamente qui
void run_extract(const char* p_cstr, size_t o, bool test) {
    std::string p(p_cstr); // Creazione sicura della stringa
    TarcResult r = Engine::extract(p, {}, test, o);
    g_ok = r.ok;
    g_message = r.message;
}

void run_list(const char* p_cstr, size_t o) {
    std::string p(p_cstr); // Creazione sicura della stringa
    TarcResult r = Engine::list(p, o);
    g_ok = r.ok;
    g_message = r.message;
}

// === FUNZIONE ISOLATA PER IL CRASH CATCHER ===
// NON dichiara NESSUN oggetto C++ (nessuna std::string locale). 
// Prende solo tipi primitivi e puntatori per superare il blocco C2712 di MSVC.
void do_safe_action(const char* p_cstr, size_t o, int action) {
    g_ok = false;
    g_message.clear();
    g_crashed = false;

    __try {
        if (action == 1) {
            UI::print_info("Estrazione in corso...");
            run_extract(p_cstr, o, false);
        } else if (action == 2) {
            UI::print_info("Contenuto dell'archivio:");
            run_list(p_cstr, o);
        } else if (action == 3) {
            UI::print_info("Verifica integrita' in corso...");
            run_extract(p_cstr, o, true);
        }
    } 
    __except(EXCEPTION_EXECUTE_HANDLER) {
        UI::print_error("ACCESS VIOLATION: Crash di memoria durante la decompressione!");
        g_crashed = true;
    }
}
// =====================================

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

    int exec_action = 0; 

    if (choice == '1') exec_action = 1;
    else if (choice == '2') exec_action = 2;
    else if (choice == '3') exec_action = 3;
    else if (choice == '4') return 0;

    // Chiamata alla funzione isolata. Convertiamo la stringa in puntatore C per sicurezza
    do_safe_action(self_path.c_str(), offset, exec_action);

    // Stampa i risultati tornando nel main sicuro
    if (!g_crashed) {
        TarcResult final_res = {g_ok, g_message};
        if (exec_action == 1) UI::print_summary(final_res, "Estrazione SFX");
        if (exec_action == 2) UI::print_summary(final_res, "Lista SFX");
        if (exec_action == 3) UI::print_summary(final_res, "Test SFX");

        if (!g_ok) {
            UI::print_error("Dettaglio: " + g_message);
        }
    }

    std::cout << "\nProcedura terminata. Premere INVIO per uscire...";
    std::cin.get();

    return g_crashed ? 1 : (g_ok ? 0 : 1);
}
