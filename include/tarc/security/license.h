#pragma once
#include <string>
#include "tarc/util/result.h"

namespace tarc::security {

// ═══════════════════════════════════════════════════════════════════════════
// LICENSE MANAGEMENT
// ═══════════════════════════════════════════════════════════════════════════

/// Valida licenza usando HMAC-SHA256
[[nodiscard]] bool is_valid_license(const std::string& key);

/// Genera ID macchina (hash hardware)
[[nodiscard]] std::string get_machine_id();

/// Path file licenza (OS-aware)
[[nodiscard]] std::string get_license_path();

/// Carica licenza salvata
[[nodiscard]] std::string load_saved_license();

/// Salva licenza su disco (con offuscamento)
[[nodiscard]] Result save_license(const std::string& key);

/// Controlla e attiva licenza (blocca se non valida)
void check_and_activate();

/// Genera timestamp corrente (per validazione scadenza)
[[nodiscard]] uint64_t get_current_timestamp();

} // namespace tarc::security
