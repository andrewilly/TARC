#pragma once
#include <string>

namespace License {

    // Valida il formato e il checksum della chiave
    bool is_valid(const std::string& key);

    // Restituisce il path del file licenza (OS-aware)
    std::string get_license_path();

    // Carica la chiave salvata dal disco
    std::string load_saved_key();

    // Salva la chiave su disco
    bool save_key(const std::string& key);

    // Controlla la licenza: carica dal disco o chiede all'utente.
    // Termina il processo se la licenza non e' valida.
    void check_and_activate();

} // namespace License
