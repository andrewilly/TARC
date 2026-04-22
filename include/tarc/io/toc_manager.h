#pragma once
#include <cstdio>
#include <vector>
#include "tarc/util/types.h"
#include "tarc/util/result.h"

namespace tarc::io {

// ═══════════════════════════════════════════════════════════════════════════
// TOC MANAGER
// ═══════════════════════════════════════════════════════════════════════════

class TOCManager {
public:
    /// Legge TOC da archivio con validazione
    [[nodiscard]] static Result read_toc(
        FILE* file,
        const Header& header,
        std::vector<FileEntry>& toc_out
    );
    
    /// Scrive TOC in archivio e aggiorna header
    [[nodiscard]] static Result write_toc(
        FILE* file,
        Header& header,
        const std::vector<FileEntry>& toc
    );
    
    /// Valida singola entry TOC
    [[nodiscard]] static Result validate_entry(const FileEntry& entry);
};

} // namespace tarc::io
