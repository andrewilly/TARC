#include "tarc/io/toc_manager.h"
#include "tarc/security/validator.h"
#include "tarc/util/constants.h"
#include <cstring>

namespace tarc::io {

// ═══════════════════════════════════════════════════════════════════════════
// HELPER: Checked I/O
// ═══════════════════════════════════════════════════════════════════════════

static Result checked_fread(FILE* f, void* buf, size_t size, const char* context) {
    if (std::fread(buf, 1, size, f) != size) {
        if (std::feof(f)) {
            return Result::error(ErrorCode::CorruptedTOC,
                std::string("EOF prematuro durante: ") + context);
        }
        return Result::error(ErrorCode::CannotReadFile,
            std::string("Errore lettura: ") + context);
    }
    return Result::success();
}

static Result checked_fwrite(FILE* f, const void* buf, size_t size, const char* context) {
    if (std::fwrite(buf, 1, size, f) != size) {
        return Result::error(ErrorCode::CannotWriteFile,
            std::string("Errore scrittura: ") + context);
    }
    return Result::success();
}

// ═══════════════════════════════════════════════════════════════════════════
// READ TOC
// ═══════════════════════════════════════════════════════════════════════════

Result TOCManager::read_toc(
    FILE* file,
    const Header& header,
    std::vector<FileEntry>& toc_out
) {
    // Validazione header
    auto file_size = [file]() -> size_t {
        auto pos = std::ftell(file);
        std::fseek(file, 0, SEEK_END);
        auto size = std::ftell(file);
        std::fseek(file, pos, SEEK_SET);
        return size;
    }();
    
    auto header_check = security::validate_header(header, file_size);
    if (header_check.failed()) {
        return header_check;
    }
    
    // Validazione numero file
    auto count_check = security::validate_file_count(header.file_count);
    if (count_check.failed()) {
        return count_check;
    }
    
    // Seek a TOC
    if (std::fseek(file, static_cast<long>(header.toc_offset), SEEK_SET) != 0) {
        return Result::error(ErrorCode::InvalidOffset,
            "Impossibile seekare a TOC offset");
    }
    
    // Pre-alloca per performance
    toc_out.clear();
    toc_out.reserve(std::min<size_t>(header.file_count, constants::VECTOR_RESERVE_COUNT));
    
    // Leggi entries
    for (uint32_t i = 0; i < header.file_count; ++i) {
        FileEntry entry;
        
        // Leggi metadata
        auto meta_result = checked_fread(file, &entry.meta, sizeof(Entry), 
                                         "entry metadata");
        if (meta_result.failed()) return meta_result;
        
        // Validazione lunghezza nome
        auto len_check = security::validate_filename_length(entry.meta.name_len);
        if (len_check.failed()) {
            return len_check;
        }
        
        // Leggi nome file
        std::vector<char> name_buf(entry.meta.name_len + 1, '\0');
        auto name_result = checked_fread(file, name_buf.data(), 
                                         entry.meta.name_len, "filename");
        if (name_result.failed()) return name_result;
        
        entry.name = std::string(name_buf.data());
        
        // Validazione entry completa
        auto entry_check = validate_entry(entry);
        if (entry_check.failed()) {
            return entry_check;
        }
        
        toc_out.push_back(std::move(entry));
    }
    
    return Result::success();
}

// ═══════════════════════════════════════════════════════════════════════════
// WRITE TOC
// ═══════════════════════════════════════════════════════════════════════════

Result TOCManager::write_toc(
    FILE* file,
    Header& header,
    const std::vector<FileEntry>& toc
) {
    // Flush e ottieni posizione corrente
    std::fflush(file);
    long toc_pos = std::ftell(file);
    if (toc_pos == -1) {
        return Result::error(ErrorCode::CannotWriteFile,
            "Impossibile determinare posizione TOC");
    }
    
    // Aggiorna header
    header.toc_offset = static_cast<uint64_t>(toc_pos);
    header.file_count = static_cast<uint32_t>(toc.size());
    
    // Scrivi entries
    for (const auto& entry : toc) {
        // Valida entry
        auto validation = validate_entry(entry);
        if (validation.failed()) {
            return validation;
        }
        
        // Scrivi metadata
        auto meta_result = checked_fwrite(file, &entry.meta, sizeof(Entry),
                                          "entry metadata");
        if (meta_result.failed()) return meta_result;
        
        // Scrivi nome
        auto name_result = checked_fwrite(file, entry.name.c_str(), 
                                          entry.meta.name_len, "filename");
        if (name_result.failed()) return name_result;
    }
    
    // Scrivi header aggiornato all'inizio
    if (std::fseek(file, 0, SEEK_SET) != 0) {
        return Result::error(ErrorCode::CannotWriteFile,
            "Impossibile seekare a inizio file");
    }
    
    auto header_result = checked_fwrite(file, &header, sizeof(Header), "header");
    if (header_result.failed()) return header_result;
    
    // Torna alla fine
    std::fseek(file, 0, SEEK_END);
    std::fflush(file);
    
    return Result::success();
}

// ═══════════════════════════════════════════════════════════════════════════
// VALIDATE ENTRY
// ═══════════════════════════════════════════════════════════════════════════

Result TOCManager::validate_entry(const FileEntry& entry) {
    // Valida lunghezza nome
    if (entry.meta.name_len != entry.name.size()) {
        return Result::error(ErrorCode::CorruptedTOC,
            "Mismatch lunghezza nome file");
    }
    
    auto len_check = security::validate_filename_length(entry.meta.name_len);
    if (len_check.failed()) return len_check;
    
    // Valida timestamp
    if (!security::is_valid_timestamp(entry.meta.timestamp)) {
        return Result::error(ErrorCode::TimestampOutOfRange,
            "Timestamp non valido: " + std::to_string(entry.meta.timestamp));
    }
    
    // Valida codec
    if (entry.meta.codec > static_cast<uint8_t>(Codec::BROTLI)) {
        return Result::error(ErrorCode::CodecNotSupported,
            "Codec non supportato: " + std::to_string(entry.meta.codec));
    }
    
    // Valida path (sicurezza)
    if (!security::is_safe_relative_path(entry.name)) {
        return Result::error(ErrorCode::PathTraversal,
            "Path non sicuro rilevato: " + entry.name);
    }
    
    return Result::success();
}

} // namespace tarc::io
