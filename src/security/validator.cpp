#include "tarc/security/validator.h"
#include "tarc/util/constants.h"
#include "tarc/util/types.h"
#include <algorithm>
#include <cstring>

namespace fs = std::filesystem;

namespace tarc::security {

// ═══════════════════════════════════════════════════════════════════════════
// PATH VALIDATION
// ═══════════════════════════════════════════════════════════════════════════

Result validate_extraction_path(const std::string& path, const fs::path& base_dir) {
    try {
        fs::path p(path);
        
        // Blocca path assoluti
        if (p.is_absolute()) {
            return Result::error(ErrorCode::PathTraversal, 
                "Path assoluto non consentito: " + path);
        }
        
        // Costruisce path canonico
        fs::path target = fs::weakly_canonical(base_dir / p);
        fs::path base_canonical = fs::weakly_canonical(base_dir);
        
        // Verifica che target sia sotto base_dir
        auto [target_it, base_it] = std::mismatch(
            target.begin(), target.end(),
            base_canonical.begin(), base_canonical.end()
        );
        
        if (base_it != base_canonical.end()) {
            return Result::error(ErrorCode::PathTraversal,
                "Path traversal rilevato: " + path);
        }
        
        return Result::success();
        
    } catch (const std::exception& e) {
        return Result::error(ErrorCode::PathTraversal, 
            "Errore validazione path: " + std::string(e.what()));
    }
}

std::string sanitize_path(const std::string& path) {
    std::string clean = path;
    
    // Normalizza separatori
    std::replace(clean.begin(), clean.end(), '\\', '/');
    
    // Rimuovi componenti pericolosi
    auto is_dangerous = [](const std::string& component) {
        return component == ".." || component == "." || component.empty();
    };
    
    std::string result;
    size_t start = 0;
    while (start < clean.size()) {
        size_t end = clean.find('/', start);
        if (end == std::string::npos) end = clean.size();
        
        std::string component = clean.substr(start, end - start);
        if (!is_dangerous(component)) {
            if (!result.empty()) result += '/';
            result += component;
        }
        
        start = end + 1;
    }
    
    return result;
}

bool is_safe_relative_path(const std::string& path) {
    if (path.empty()) return false;
    if (path[0] == '/' || path[0] == '\\') return false;
    
    // Blocca drive letters Windows (C:, D:, ecc.)
    if (path.size() >= 2 && path[1] == ':') return false;
    
    // Blocca UNC paths (\\server\share)
    if (path.size() >= 2 && path[0] == '\\' && path[1] == '\\') return false;
    
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// SIZE VALIDATION
// ═══════════════════════════════════════════════════════════════════════════

Result validate_chunk_size(size_t size) {
    if (size > constants::MAX_CHUNK_SIZE) {
        return Result::error(ErrorCode::ChunkSizeExceeded,
            "Chunk troppo grande: " + std::to_string(size) + " byte (max: " +
            std::to_string(constants::MAX_CHUNK_SIZE) + ")");
    }
    return Result::success();
}

Result validate_filename_length(size_t len) {
    if (len > constants::MAX_FILENAME_LENGTH) {
        return Result::error(ErrorCode::FilenameToolong,
            "Nome file troppo lungo: " + std::to_string(len) + " caratteri (max: " +
            std::to_string(constants::MAX_FILENAME_LENGTH) + ")");
    }
    return Result::success();
}

Result validate_file_count(uint32_t count) {
    if (count > constants::MAX_FILE_COUNT) {
        return Result::error(ErrorCode::TooManyFiles,
            "Troppi file: " + std::to_string(count) + " (max: " +
            std::to_string(constants::MAX_FILE_COUNT) + ")");
    }
    return Result::success();
}

// ═══════════════════════════════════════════════════════════════════════════
// TIMESTAMP VALIDATION
// ═══════════════════════════════════════════════════════════════════════════

uint64_t sanitize_timestamp(uint64_t ts) {
    return std::clamp(ts, constants::MIN_TIMESTAMP, constants::MAX_TIMESTAMP);
}

bool is_valid_timestamp(uint64_t ts) {
    return ts >= constants::MIN_TIMESTAMP && ts <= constants::MAX_TIMESTAMP;
}

// ═══════════════════════════════════════════════════════════════════════════
// ARCHIVE VALIDATION
// ═══════════════════════════════════════════════════════════════════════════

Result validate_header(const Header& header, size_t file_size) {
    // Verifica magic number
    if (std::memcmp(header.magic, constants::MAGIC, 4) != 0) {
        return Result::error(ErrorCode::InvalidHeader,
            "Magic number non valido (archivio corrotto o non TARC)");
    }
    
    // Verifica versione
    if (header.version != constants::VERSION) {
        return Result::error(ErrorCode::InvalidHeader,
            "Versione archivio non supportata: " + std::to_string(header.version));
    }
    
    // Valida TOC offset
    return validate_toc_offset(header.toc_offset, file_size);
}

Result validate_toc_offset(uint64_t offset, size_t file_size) {
    // TOC deve essere dopo header
    if (offset < sizeof(Header)) {
        return Result::error(ErrorCode::InvalidOffset,
            "TOC offset troppo piccolo: " + std::to_string(offset));
    }
    
    // TOC deve essere dentro il file
    if (offset >= file_size) {
        return Result::error(ErrorCode::InvalidOffset,
            "TOC offset fuori dal file: " + std::to_string(offset) + 
            " (dimensione file: " + std::to_string(file_size) + ")");
    }
    
    return Result::success();
}

} // namespace tarc::security
