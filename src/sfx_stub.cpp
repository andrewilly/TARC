// ============================================================================
// TARC SFX Stub — Self-Extracting Archive Launcher
// ============================================================================
// This file is compiled as a separate executable (tarc_sfx_stub).
// It is then concatenated with a .strk archive to create a self-extracting .exe:
//
//   [Stub EXE][TARC .strk Archive][SfxTrailer (24 bytes)]
//
// On startup, the stub:
//   1. Opens itself in binary mode
//   2. Reads the last 24 bytes (SfxTrailer)
//   3. Validates the magic "TARC_SFX"
//   4. Extracts the TARC archive to the current directory (or --output-dir)
//
// Usage:
//   archive_sfx.exe                  Extracts to current folder
//   archive_sfx.exe --output-dir X   Extracts to folder X
//   archive_sfx.exe --force          Overwrites existing files
//   archive_sfx.exe --help           Shows help
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
#elif defined(__APPLE__)
    #include <mach-o/dyld.h>
    #include <unistd.h>
#else
    #include <unistd.h>
#endif

namespace fs = std::filesystem;

// ============================================================================
// Extract the TARC archive embedded in the SFX executable
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

    // Show info
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

    // Show summary
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

    // Show banner
    std::cout << Color::CYAN << Color::BOLD
              << "  TARC STRIKE — Self-Extracting Archive\n"
              << Color::RESET << "\n";

    // Parse minimal options
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

    // Show warning if --force is not specified and files already exist
    if (!overwrite && !output_dir.empty()) {
        fs::path out_path(output_dir);
        if (fs::exists(out_path)) {
            std::cout << Color::YELLOW << "[!] Output directory exists: " << output_dir
                      << "\n    Use --force to overwrite existing files."
                      << Color::RESET << "\n\n";
        }
    }

    // Get the path of the executable itself
    std::string self_path = IO::get_self_path();
    if (self_path.empty()) {
        // Fallback: use argv[0]
        self_path = argv[0];
    }

    // Verify that the file exists
    if (!fs::exists(self_path)) {
        std::cerr << "Error: cannot find self executable: " << self_path << "\n";
        return 1;
    }

    return extract_embedded(self_path, output_dir, overwrite);
}
