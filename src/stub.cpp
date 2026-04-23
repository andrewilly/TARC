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
// Servono per passare i dati fuori dal blocco __try senza usare variabili locali
bool g_ok = false;
std::string g_message = "";
bool g_crashed = false;

// === FUNZIONI SCUDO (WRAPPER) ===
// Eseguono il codice che restituisce oggetti C++ complessi (TarcResult) fuori dal blocco __try
void run_extract(const std::string& p, size_t o, bool test) {
    TarcResult r = Engine::extract(p, {}, test, o);
    g_ok = r.ok;
    g_message = r.message;
}

void run_list(const std::string& p, size_t o) {
    TarcResult r = Engine::list(p, o);
    g_ok = r.ok;
    g_message = r.message;
}
// =====================================

void show_stub_header() {
    std::cout << Color::CYAN << "==========================================\n";
    std::cout << "          TARC SELF-EXTRACTOR             \n";
    std::cout << "       Powered by Strike Engine v2.04     \n";
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

    int exec_action = 0; // 0=Niente, 1=Estrai, 2=Lista, 3=Test

    if (choice == '1') exec_action = 1;
    else if (choice == '2') exec_action = 2;
    else if (choice == '3') exec_action = 3;
    else if (choice == '4') return 0;

    // === BLOCCO TRY DI WINDOWS (ORA LEGALE PER MSVC) ===
    __try {
        if (exec_action == 1) {
            UI::print_info("Estrazione in corso...");
            run_extract(self_path, offset, false);
        } else if (exec_action == 2) {
            UI::print_info("Contenuto dell'archivio:");
            run_list(self_path, offset);
        } else if (exec_action == 3) {
            UI::print_info("Verifica integrita' in corso...");
            run_extract(self_path, offset, true);
        }
    } 
    __except(EXCEPTION_EXECUTE_HANDLER) {
        UI::print_error("ACCESS VIOLATION: Crash durante la decompressione!");
        g_crashed = true;
    }
    // ====================================================

    // Stampa i risultati finali FUORI dal blocco __try
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
