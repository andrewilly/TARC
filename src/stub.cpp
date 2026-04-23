// ═══════════════════════════════════════════════════════════════════════════════
// TARC SELF-EXTRACTOR STUB v2.04
// ═══════════════════════════════════════════════════════════════════════════════
//
// Questo stub viene concatenato all'archivio .strk per creare un SFX.
// Layout del file SFX finale:
//
//   [  STUB EXE  ][  ARCHIVIO .strk  ][  SFXTrailer (12 byte)  ]
//                                  ↑                ↑
//                          archive_offset         "TSFX" magic
//
// Il trailer contiene l'offset esatto dell'archivio, cosi' lo stub
// non deve scansionare tutto il file cercando "TRC2".
//
// COMPILAZIONE (MSVC, da Developer Command Prompt):
//   cl /std:c++17 /O2 /EHsc /utf-8 /Fe:tarc_sfx_stub.exe stub.cpp ^
//      ..\src\ui.cpp ..\src\io.cpp ..\src\engine.cpp ..\src\license.cpp ^
//      /I..\include /I..\build\_deps\xxhash-src ^
//      ..\build\_deps\xxhash-src\xxhash.c ^
//      /link libzstd_static.lib lz4_static.lib lzma.lib brotlienc.lib brotlidec.lib brotlicommon.lib bcrypt.lib user32.lib
//
// ═══════════════════════════════════════════════════════════════════════════════

#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <cstring>
#include <cstdint>
#include <filesystem>

#include <windows.h>

#include "engine.h"
#include "ui.h"
#include "types.h"

namespace fs = std::filesystem;

// ═══════════════════════════════════════════════════════════════════════════════
// TROVA OFFSET ARCHIVIO — Approccio trailer + fallback
// ═══════════════════════════════════════════════════════════════════════════════

size_t find_archive_offset(const std::string& exe_path) {
    std::ifstream f(fs::u8path(exe_path), std::ios::binary | std::ios::ate);
    if (!f) return 0;

    auto file_size = static_cast<size_t>(f.tellg());
    if (file_size < sizeof(Header) + sizeof(SFXTrailer)) return 0;

    // ── METODO 1: Leggi il trailer SFX (ultimi 12 byte) ───────────────────
    // Il trailer contiene l'offset esatto dell'archivio ed il magic "TSFX".
    // Questo e' il metodo primario, veloce O(1) e deterministico.
    if (file_size >= sizeof(SFXTrailer)) {
        SFXTrailer trailer;
        f.seekg(static_cast<std::streamoff>(file_size - sizeof(SFXTrailer)));
        f.read(reinterpret_cast<char*>(&trailer), sizeof(trailer));

        if (memcmp(trailer.magic, SFX_TRAILER_MAGIC, 4) == 0) {
            // Valida l'offset: deve essere positivo e minore del file
            if (trailer.archive_offset > 0 && trailer.archive_offset < file_size) {
                // Verifica che all'offset ci sia davvero il magic "TRC2"
                f.seekg(static_cast<std::streamoff>(trailer.archive_offset));
                char magic[4] = {};
                f.read(magic, 4);

                if (memcmp(magic, TARC_MAGIC, 4) == 0) {
                    return trailer.archive_offset;
                }
            }
        }
    }

    // ── METODO 2: Fallback — Scansione all'indietro per "TRC2" ────────────
    // Per SFX vecchi senza trailer. Cerca la firma "TRC2" partendo dal
    // fondo del file, saltando i primi 128 KB (codice dello stub).
    // NOTA: Questo metodo e' meno affidabile e carica il file in RAM.
    std::vector<char> buffer(file_size);
    f.seekg(0, std::ios::beg);
    f.read(buffer.data(), file_size);
    f.close();

    const size_t skip_head = 128 * 1024;  // Salta i primi 128 KB dello stub
    for (size_t i = file_size - 4; i > skip_head; --i) {
        if (buffer[i] == 'T' && buffer[i+1] == 'R' &&
            buffer[i+2] == 'C' && buffer[i+3] == '2') {
            return i;
        }
    }

    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════════
// PERCORSO DELL'ESEGUIBILE — Unicode-safe
// ═══════════════════════════════════════════════════════════════════════════════

std::string get_self_path() {
    // Usa GetModuleFileNameW per supportare percorsi Unicode
    wchar_t wpath[MAX_PATH + 1] = {};
    DWORD len = GetModuleFileNameW(NULL, wpath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return "";

    // Converti wide string a UTF-8 tramite fs::path
    fs::path p = fs::path(wpath);
    return p.u8string();
}

// ═══════════════════════════════════════════════════════════════════════════════
// STUB HEADER
// ═══════════════════════════════════════════════════════════════════════════════

void show_stub_header() {
    std::cout << Color::CYAN << "==========================================\n";
    std::cout << "          TARC SELF-EXTRACTOR             \n";
    std::cout << "       Powered by Strike Engine v2.04     \n";
    std::cout << "==========================================\n" << Color::RESET;
}

// ═══════════════════════════════════════════════════════════════════════════════
// MAIN
// ═══════════════════════════════════════════════════════════════════════════════

int main(int argc, char* argv[]) {
    UI::enable_vtp();
    show_stub_header();

    // 1. Percorso dell'eseguibile corrente (Unicode-safe)
    std::string self_path = get_self_path();
    if (self_path.empty()) {
        UI::print_error("Errore: Impossibile determinare il percorso del file.");
        std::cout << "Premere INVIO per uscire...";
        std::cin.get();
        return 1;
    }

    // 2. Trova l'offset dell'archivio (trailer-first, fallback scan)
    size_t offset = find_archive_offset(self_path);

    if (offset == 0) {
        UI::print_error("Errore: Dati TRC2 non trovati. Questo non e' un archivio SFX valido.");
        std::cout << "Premere INVIO per uscire...";
        std::cin.get();
        return 1;
    }

    // 3. Menu Operativo
    std::cout << "\n" << Color::YELLOW << " OPERAZIONI DISPONIBILI:" << Color::RESET << "\n";
    std::cout << "  [1] Estrai tutto (cartella corrente)\n";
    std::cout << "  [2] Estrai in una directory...\n";
    std::cout << "  [3] Elenca files\n";
    std::cout << "  [4] Verifica integrita' (Checksum)\n";
    std::cout << "  [5] Esci\n";
    std::cout << "\nScelta > ";

    std::string input;
    std::getline(std::cin, input);
    if (input.empty()) return 0;
    char choice = input[0];

    TarcResult res;
    switch (choice) {
        case '1':
            UI::print_info("Estrazione in corso...");
            UI::progress_timer_reset();
            res = Engine::extract(self_path, {}, false, offset);
            UI::print_summary(res, "Estrazione SFX");
            break;

        case '2': {
            std::cout << "Directory di destinazione: ";
            std::string dest_dir;
            std::getline(std::cin, dest_dir);
            if (dest_dir.empty()) {
                UI::print_warning("Nessuna directory specificata. Operazione annullata.");
                break;
            }
            UI::print_info("Estrazione in " + dest_dir + "...");
            UI::progress_timer_reset();
            res = Engine::extract(self_path, {}, false, offset, false, dest_dir);
            UI::print_summary(res, "Estrazione SFX");
            break;
        }

        case '3':
            UI::print_info("Contenuto dell'archivio:");
            res = Engine::list(self_path, offset);
            if (res.ok) UI::print_summary(res, "Lista");
            else UI::print_error("Errore lettura: " + res.message);
            break;

        case '4':
            UI::print_info("Verifica integrita' in corso...");
            UI::progress_timer_reset();
            res = Engine::extract(self_path, {}, true, offset);
            UI::print_summary(res, "Test SFX");
            break;

        case '5':
            return 0;

        default:
            UI::print_warning("Scelta non valida.");
            return 0;
    }

    // 4. Gestione errori
    if (!res.ok) {
        UI::print_error("L'operazione ha riscontrato un errore:");
        UI::print_error("Dettaglio: " + res.message);
    }

    std::cout << "\nProcedura terminata. Premere INVIO per uscire...";
    std::cin.get();

    return res.ok ? 0 : 1;
}
