#include "ui.h"
#include "license.h"
#include "engine.h"
#include <string>
#include <vector>

int main(int argc, char* argv[]) {
    UI::enable_vtp();
    UI::show_banner();
    License::check_and_activate();

    if (argc < 3) {
        UI::show_help();
        return 0;
    }

    std::string cmd = argv[1];
    std::string arch = argv[2];

    if (cmd == "-c") {
        std::vector<std::string> files;
        for (int i = 3; i < argc; ++i) files.push_back(argv[i]);
        auto res = Engine::compress(arch, files, false, 3);
        if (!res.ok) UI::print_error(res.message);
    } else if (cmd == "-l") {
        Engine::list(arch);
    } else if (cmd == "-x") {
        Engine::extract(arch);
    } else {
        UI::print_error("Comando sconosciuto.");
    }

    return 0;
}