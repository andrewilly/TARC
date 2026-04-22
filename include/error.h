#pragma once
#include <string>
#include <expected>
#include <source_location>

namespace tarc {

// Categorie di errore
enum class ErrorCategory {
    SUCCESS = 0,
    IO_ERROR,
    MEMORY_ERROR,
    CORRUPTION_ERROR,
    PERMISSION_ERROR,
    FORMAT_ERROR,
    COMPRESSION_ERROR,
    DECOMPRESSION_ERROR,
    DEDUP_ERROR,
    INTERNAL_ERROR
};

// Codici errore specifici
enum class ErrorCode {
    OK = 0,
    
    // IO Errori (1-99)
    FILE_NOT_FOUND = 1,
    FILE_OPEN_FAILED = 2,
    FILE_READ_FAILED = 3,
    FILE_WRITE_FAILED = 4,
    FILE_SEEK_FAILED = 5,
    FILE_CORRUPTED = 6,
    
    // Memoria Errori (100-199)
    OUT_OF_MEMORY = 100,
    BUFFER_TOO_SMALL = 101,
    ALLOCATION_FAILED = 102,
    
    // Compressione Errori (200-299)
    LZMA_MEM_ERROR = 200,
    LZMA_OPTIONS_ERROR = 201,
    LZMA_DATA_ERROR = 202,
    LZMA_BUF_ERROR = 203,
    LZMA_FORMAT_ERROR = 204,
    
    // Decompressione Errori (300-399)
    LZMA_DECOMPRESS_MEM_ERROR = 300,
    LZMA_DECOMPRESS_DATA_ERROR = 301,
    LZMA_DECOMPRESS_BUF_ERROR = 302,
    LZMA_DECOMPRESS_FORMAT_ERROR = 303,
    
    // Integrità Errori (400-499)
    CHECKSUM_MISMATCH = 400,
    MAGIC_MISMATCH = 401,
    VERSION_MISMATCH = 402,
    TOC_CORRUPTED = 403,
    
    // Permission Errori (500-599)
    ACCESS_DENIED = 500,
    READ_ONLY = 501,
    
    // Dedup Errori (600-699)
    HASH_COLLISION = 600,
    DUPLICATE_REF_INVALID = 601,
    
    // Interni (900-999)
    UNKNOWN_ERROR = 999
};

// Informazioni errore dettagliate
struct ErrorInfo {
    ErrorCode code = ErrorCode::OK;
    ErrorCategory category = ErrorCategory::SUCCESS;
    std::string message;
    std::string file;
    int line = 0;
    std::string function;
    
    ErrorInfo() = default;
    
    ErrorInfo(ErrorCode c, const std::string& msg,
              const std::source_location& loc = std::source_location::current())
        : code(c)
        , category(get_category_from_code(c))
        , message(msg)
        , file(loc.file_name())
        , line(loc.line())
        , function(loc.function_name()) {}
    
    static ErrorCategory get_category_from_code(ErrorCode code) {
        int val = static_cast<int>(code);
        if (val >= 1 && val <= 99) return ErrorCategory::IO_ERROR;
        if (val >= 100 && val <= 199) return ErrorCategory::MEMORY_ERROR;
        if (val >= 200 && val <= 299) return ErrorCategory::COMPRESSION_ERROR;
        if (val >= 300 && val <= 399) return ErrorCategory::DECOMPRESSION_ERROR;
        if (val >= 400 && val <= 499) return ErrorCategory::CORRUPTION_ERROR;
        if (val >= 500 && val <= 599) return ErrorCategory::PERMISSION_ERROR;
        if (val >= 600 && val <= 699) return ErrorCategory::DEDUP_ERROR;
        return ErrorCategory::INTERNAL_ERROR;
    }
    
    std::string to_string() const {
        std::string result = "[" + std::to_string(static_cast<int>(code)) + "] ";
        result += message;
        result += " at " + file + ":" + std::to_string(line);
        return result;
    }
    
    bool is_recoverable() const {
        return code == ErrorCode::OUT_OF_MEMORY || 
               code == ErrorCode::BUFFER_TOO_SMALL ||
               code == ErrorCode::LZMA_MEM_ERROR ||
               code == ErrorCode::LZMA_DECOMPRESS_MEM_ERROR;
    }
};

// Tipo Result per funzioni che possono fallire
template<typename T>
using Result = std::expected<T, ErrorInfo>;

// Macro per catturare posizione errore
#define TARC_TRY(expr) \
    do { \
        auto _result = (expr); \
        if (!_result.has_value()) { \
            return std::unexpected(_result.error()); \
        } \
    } while(0)

#define TARC_RETURN_ERROR(code, msg) \
    return std::unexpected(ErrorInfo(code, msg, std::source_location::current()))

// Helper per convertire errori sistema in ErrorCode
inline ErrorCode system_error_to_code(int sys_err) {
    switch (sys_err) {
        case ENOENT: return ErrorCode::FILE_NOT_FOUND;
        case EACCES: return ErrorCode::ACCESS_DENIED;
        case EPERM: return ErrorCode::ACCESS_DENIED;
        case ENOMEM: return ErrorCode::OUT_OF_MEMORY;
        case ENOSPC: return ErrorCode::FILE_WRITE_FAILED;
        case EIO: return ErrorCode::FILE_READ_FAILED;
        default: return ErrorCode::UNKNOWN_ERROR;
    }
}

inline std::string system_error_to_string(int sys_err) {
    return std::string(strerror(sys_err));
}

} // namespace tarc
