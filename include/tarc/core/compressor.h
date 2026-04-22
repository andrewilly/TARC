#pragma once
#include <vector>
#include <string>
#include "tarc/util/result.h"

namespace tarc::core {

// ═══════════════════════════════════════════════════════════════════════════
// COMPRESSOR
// ═══════════════════════════════════════════════════════════════════════════

class Compressor {
public:
    struct Options {
        int compression_level = 3;
        bool append_mode = false;
        bool create_sfx = false;
    };
    
    /// Comprime file in archivio solid
    [[nodiscard]] Result compress(
        const std::string& archive_path,
        const std::vector<std::string>& input_paths,
        const Options& options
    );
    
private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace tarc::core
