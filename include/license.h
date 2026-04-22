#pragma once
#include <string>

namespace License {

    // ─── INTERVENTO #24: VALIDAZIONE ROBUSTA ───────────────────────────────────
    // Nuovo formato chiave: TARC-XXXXXXXX-XXXXXXXX-XXXXXXXX
    //   Gruppo 1-2: identificativo utente/prodotto (8 hex ciascuno)
    //   Gruppo 3: checksum XXH64(gruppo1 + "-" + gruppo2 + SALT) troncato a 32 bit
    //
    // Il vecchio formato (sum % 7) non e' piu' accettato.

    // Valida il formato e il checksum XXH64 della chiave
    bool is_valid(const std::string& key);

    // Restituisce il path del file licenza (OS-aware, Unicode-safe)
    std::string get_license_path();

    // Carica la chiave salvata dal disco
    std::string load_saved_key();

    // Salva la chiave su disco
    bool save_key(const std::string& key);

    // Controlla la licenza: carica dal disco o chiede all'utente.
    // Include protezione brute-force (delay crescente dopo tentativi falliti).
    // Termina il processo se la licenza non e' valida.
    void check_and_activate();

} // namespace License
