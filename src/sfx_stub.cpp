// ============================================================================
// TARC SFX Stub — Self-Extracting Archive Launcher
// ============================================================================
// Questo file viene compilato come eseguibile separato (tarc_sfx_stub).
// Viene poi concatenato con un archivio .strk per creare un .exe autoestraente:
//
//   [Stub EXE][Archivio TARC .strk][SfxTrailer (24 byte)]
//
// All'avvio, il stub:
//   1. Apre se stesso in modalita' binaria
//   2. Legge gli ultimi 24 byte (SfxTrailer)
//   3. Valida il magic "TARC_SFX"
//   4. Estrae l'archivio TARC nella directory corrente (o --output-dir)
//
// Uso:
//   archive_sfx.exe                  Estrae nella cartella corrente
//   archive_sfx.exe --output-dir X   Estrae nella cartella X
//   archive_sfx.exe --force          Sovrascrive file esistenti
//   archive_sfx.exe --help           Mostra help
// ============================================================================

#include "types.h"
#include "io.h"
#include "ui.h"
#include "engine.h"

#include <iostream>
#include <string>
#include <vector>
#include <cstring>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
    #include <windows.h>
    #include <shellapi.h>
#else
    #include <unistd.h>
#endif

namespace fs = std::filesystem;

// ============================================================================
// Ottieni il percorso completo dell'eseguibile corrente
// ============================================================================
static std::string get_self_path() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return "";
    // Converti da wide char a UTF-8
    int narrow_len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    if (narrow_len <= 0) return "";
    std::string result(narrow_len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, &result[0], narrow_len, nullptr, nullptr);
    return result;
#else
    // Linux/macOS: leggi il symlink /proc/self/exe
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0) {
        // Fallback: usa argv[0] passato dal main
        return "";
    }
    buf[len] = '\0';
    return std::string(buf);
#endif
}

// ============================================================================
// Estrai l'archivio TARC embeddato nell'eseguibile SFX
// ============================================================================
static int extract_embedded(const std::string& self_path,
                            const std::string& output_dir,
                            bool overwrite) {
    // Apri l'eseguibile stesso in lettura binaria
    std::ifstream self(self_path, std::ios::binary);
    if (!self) {
        std::cerr << "Error: cannot open self: " << self_path << "\n";
        return 1;
    }

    // Determina la dimensione del file
    self.seekg(0, std::ios::end);
    auto file_size = self.tellg();
    if (file_size < static_cast<std::streamoff>(SFX_TRAILER_SIZE)) {
        std::cerr << "Error: file too small to be a valid SFX archive.\n";
        return 1;
    }

    // Leggi il trailer (ultimi 24 byte)
    SfxTrailer trailer;
    self.seekg(-static_cast<std::streamoff>(SFX_TRAILER_SIZE), std::ios::end);
    self.read(reinterpret_cast<char*>(&trailer), SFX_TRAILER_SIZE);
    if (self.gcount() != SFX_TRAILER_SIZE) {
        std::cerr << "Error: cannot read SFX trailer.\n";
        return 1;
    }

    // Valida il magic
    if (std::memcmp(trailer.magic, SFX_MAGIC, 8) != 0) {
        std::cerr << "Error: not a valid TARC SFX archive (bad magic).\n";
        return 1;
    }

    // Validazione offset e dimensione
    if (trailer.archive_offset >= static_cast<uint64_t>(file_size)) {
        std::cerr << "Error: SFX archive offset is invalid.\n";
        return 1;
    }
    if (trailer.archive_offset + trailer.archive_size >
        static_cast<uint64_t>(file_size) - SFX_TRAILER_SIZE) {
        std::cerr << "Error: SFX archive size is invalid.\n";
        return 1;
    }

    // Crea un file temporaneo con l'archivio TARC
    fs::path temp_dir = fs::temp_directory_path();
    fs::path temp_archive = temp_dir / "tarc_sfx_temp.strk";

    {
        std::ofstream out(temp_archive, std::ios::binary);
        if (!out) {
            std::cerr << "Error: cannot create temporary archive file.\n";
            return 1;
        }

        // Leggi l'archivio embeddato
        self.clear();
        self.seekg(static_cast<std::streamoff>(trailer.archive_offset));
        if (!self) {
            std::cerr << "Error: cannot seek to archive data.\n";
            return 1;
        }

        // Copia l'archivio nel file temporaneo (chunk di 1MB per sicurezza)
        const size_t BUF_SIZE = 1024 * 1024;
        std::vector<char> buf(BUF_SIZE);
        uint64_t remaining = trailer.archive_size;

        while (remaining > 0) {
            size_t to_read = static_cast<size_t>(std::min(remaining, static_cast<uint64_t>(BUF_SIZE)));
            self.read(buf.data(), to_read);
            if (self.gcount() != static_cast<std::streamsize>(to_read)) {
                std::cerr << "Error: failed to read embedded archive data.\n";
                // Cleanup
                out.close();
                std::error_code ec;
                fs::remove(temp_archive, ec);
                return 1;
            }
            out.write(buf.data(), to_read);
            remaining -= to_read;
        }
        out.flush();
        out.close();

        if (!out.good() && remaining != 0) {
            std::cerr << "Error: failed to write temporary archive file.\n";
            std::error_code ec;
            fs::remove(temp_archive, ec);
            return 1;
        }
    }
    self.close();

    // Mostra info
    std::cout << Color::CYAN << "TARC SFX — Self-Extracting Archive\n"
              << Color::DIM << "  Embedded archive: "
              << (trailer.archive_size / (1024 * 1024)) << " MB\n"
              << Color::RESET;

    // Configura opzioni di estrazione
    ExtractOptions xopts;
    xopts.test_only = false;
    xopts.flat_mode = false;
    xopts.verify = true;
    xopts.overwrite = overwrite;
    if (!output_dir.empty()) {
        xopts.output_dir = output_dir;
    }

    // Estrai usando il motore TARC
    auto start_time = TarcUtil::safe_now();
    auto result = Engine::extract(temp_archive.string(), {}, xopts);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        TarcUtil::safe_now() - start_time
    );

    // Cleanup file temporaneo
    std::error_code ec;
    fs::remove(temp_archive, ec);
    if (ec) {
        std::cerr << Color::YELLOW << "Warning: could not delete temporary file: "
                  << temp_archive.string() << Color::RESET << "\n";
    }

    // Mostra riepilogo
    std::cout << "\n";
    UI::print_summary(result, "SFX Extract", elapsed);

    if (!output_dir.empty()) {
        std::cout << Color::DIM << "  Output: " << output_dir << Color::RESET << "\n";
    }

    return result.ok ? 0 : 1;
}

// ============================================================================
// Main SFX
// ============================================================================
int main(int argc, char* argv[]) {
    UI::enable_vtp();

    // Mostra banner
    std::cout << Color::CYAN << Color::BOLD
              << "  TARC STRIKE — Self-Extracting Archive\n"
              << Color::RESET << "\n";

    // Parse opzioni minime
    std::string output_dir;
    bool overwrite = false;
    bool show_help = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            show_help = true;
        } else if (arg == "--output-dir" && i + 1 < argc) {
            output_dir = argv[++i];
        } else if (arg == "--force" || arg == "-y") {
            overwrite = true;
        }
    }

    if (show_help) {
        std::cout << Color::BOLD << "Usage: " << Color::BRIGHT_WHITE
                  << fs::path(argv[0]).filename().string() << " [options]"
                  << Color::RESET << "\n\n";
        std::cout << "Options:\n";
        std::cout << "  " << Color::WHITE << "--output-dir <path>"
                  << Color::RESET << "  Extract to directory (default: current)\n";
        std::cout << "  " << Color::WHITE << "--force, -y"
                  << Color::RESET << "          Overwrite existing files\n";
        std::cout << "  " << Color::WHITE << "--help, -h"
                  << Color::RESET << "          Show this help\n";
        return 0;
    }

    // Mostra avviso se non si specifica --force e ci sono file gia' esistenti
    if (!overwrite && !output_dir.empty()) {
        fs::path out_path(output_dir);
        if (fs::exists(out_path)) {
            std::cout << Color::YELLOW << "[!] Output directory exists: " << output_dir
                      << "\n    Use --force to overwrite existing files."
                      << Color::RESET << "\n\n";
        }
    }

    // Ottieni il percorso dell'eseguibile stesso
    std::string self_path = get_self_path();
    if (self_path.empty()) {
        // Fallback: usa argv[0]
        self_path = argv[0];
    }

    // Verifica che il file esista
    if (!fs::exists(self_path)) {
        std::cerr << "Error: cannot find self executable: " << self_path << "\n";
        return 1;
    }

    return extract_embedded(self_path, output_dir, overwrite);
}
