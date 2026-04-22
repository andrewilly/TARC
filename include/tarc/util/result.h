#pragma once
#include <string>
#include <cstdint>

namespace tarc {

// ═══════════════════════════════════════════════════════════════════════════
// ERROR CODES
// ═══════════════════════════════════════════════════════════════════════════

enum class ErrorCode {
    Success = 0,
    
    // File I/O
    FileNotFound,
    CannotOpenFile,
    CannotReadFile,
    CannotWriteFile,
    DiskFull,
    PermissionDenied,
    
    // Archivio
    InvalidHeader,
    CorruptedTOC,
    CorruptedChunk,
    InvalidOffset,
    ChecksumMismatch,
    
    // Validazione
    FilenameToolong,
    PathTraversal,
    ChunkSizeExceeded,
    TimestampOutOfRange,
    TooManyFiles,
    
    // Compressione/Decompressione
    CompressionFailed,
    DecompressionFailed,
    CodecNotSupported,
    
    // Licenza
    InvalidLicense,
    LicenseExpired,
    CannotSaveLicense,
    
    // Generico
    UnknownError,
    OutOfMemory,
    NotImplemented
};

// ═══════════════════════════════════════════════════════════════════════════
// RESULT STRUCT
// ═══════════════════════════════════════════════════════════════════════════

struct Result {
    ErrorCode   code = ErrorCode::Success;
    std::string message;
    uint64_t    bytes_in = 0;
    uint64_t    bytes_out = 0;
    
    [[nodiscard]] bool ok() const noexcept { 
        return code == ErrorCode::Success; 
    }
    
    [[nodiscard]] bool failed() const noexcept { 
        return code != ErrorCode::Success; 
    }
    
    // Factory methods
    static Result success(const std::string& msg = "") {
        return {ErrorCode::Success, msg, 0, 0};
    }
    
    static Result error(ErrorCode code, const std::string& msg) {
        return {code, msg, 0, 0};
    }
    
    static Result with_stats(uint64_t in, uint64_t out) {
        Result r;
        r.bytes_in = in;
        r.bytes_out = out;
        return r;
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// ERROR CODE TO STRING
// ═══════════════════════════════════════════════════════════════════════════

inline const char* error_code_to_string(ErrorCode code) {
    switch (code) {
        case ErrorCode::Success: return "Success";
        case ErrorCode::FileNotFound: return "File non trovato";
        case ErrorCode::CannotOpenFile: return "Impossibile aprire il file";
        case ErrorCode::CannotReadFile: return "Errore lettura file";
        case ErrorCode::CannotWriteFile: return "Errore scrittura file";
        case ErrorCode::DiskFull: return "Disco pieno";
        case ErrorCode::PermissionDenied: return "Permesso negato";
        case ErrorCode::InvalidHeader: return "Header archivio non valido";
        case ErrorCode::CorruptedTOC: return "TOC corrotto";
        case ErrorCode::CorruptedChunk: return "Chunk corrotto";
        case ErrorCode::InvalidOffset: return "Offset non valido";
        case ErrorCode::ChecksumMismatch: return "Checksum non corrispondente";
        case ErrorCode::FilenameToolong: return "Nome file troppo lungo";
        case ErrorCode::PathTraversal: return "Path traversal rilevato";
        case ErrorCode::ChunkSizeExceeded: return "Dimensione chunk eccessiva";
        case ErrorCode::TimestampOutOfRange: return "Timestamp fuori range";
        case ErrorCode::TooManyFiles: return "Troppi file nell'archivio";
        case ErrorCode::CompressionFailed: return "Compressione fallita";
        case ErrorCode::DecompressionFailed: return "Decompressione fallita";
        case ErrorCode::CodecNotSupported: return "Codec non supportato";
        case ErrorCode::InvalidLicense: return "Licenza non valida";
        case ErrorCode::LicenseExpired: return "Licenza scaduta";
        case ErrorCode::CannotSaveLicense: return "Impossibile salvare licenza";
        case ErrorCode::UnknownError: return "Errore sconosciuto";
        case ErrorCode::OutOfMemory: return "Memoria insufficiente";
        case ErrorCode::NotImplemented: return "Funzionalità non implementata";
        default: return "Codice errore sconosciuto";
    }
}

} // namespace tarc
