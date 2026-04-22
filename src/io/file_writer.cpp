#include "tarc/io/file_reader.h"
#include "tarc/security/validator.h"
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace tarc::io {

Result write_file_to_disk(
    const std::string& path,
    const void* data,
    size_t size,
    uint64_t timestamp
) {
    // Validazione sicurezza: path traversal
    auto validation = security::validate_extraction_path(path);
    if (validation.failed()) {
        return validation;
    }
    
    try {
        fs::path file_path(path);
        
        // Crea directory parent se necessario
        if (file_path.has_parent_path()) {
            std::error_code ec;
            fs::create_directories(file_path.parent_path(), ec);
            if (ec) {
                return Result::error(ErrorCode::CannotWriteFile,
                    "Impossibile creare directory: " + ec.message());
            }
        }
        
        // Scrivi file
        std::ofstream out(file_path, std::ios::binary);
        if (!out) {
            return Result::error(ErrorCode::CannotWriteFile,
                "Impossibile creare file: " + path);
        }
        
        if (size > 0 && data != nullptr) {
            out.write(static_cast<const char*>(data), size);
            if (!out) {
                return Result::error(ErrorCode::DiskFull,
                    "Errore scrittura (disco pieno?): " + path);
            }
        }
        
        out.close();
        
        // Imposta timestamp (sanitizzato)
        uint64_t safe_ts = security::sanitize_timestamp(timestamp);
        
        try {
            auto sys_time = std::chrono::system_clock::from_time_t(
                static_cast<std::time_t>(safe_ts)
            );
            
            std::error_code ec;
            fs::last_write_time(
                file_path,
                fs::file_time_type(sys_time.time_since_epoch()),
                ec
            );
            // Ignora errori timestamp (non critico)
        } catch (...) {
            // Timestamp non supportato dal filesystem - ignora
        }
        
        return Result::success();
        
    } catch (const std::exception& e) {
        return Result::error(ErrorCode::CannotWriteFile,
            std::string("Errore: ") + e.what());
    }
}

} // namespace tarc::io
