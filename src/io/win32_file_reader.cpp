#include "tarc/io/file_reader.h"
#include "tarc/security/checksum.h"
#include "tarc/util/constants.h"
#include <filesystem>

#ifdef _WIN32
    #include <windows.h>

namespace tarc::io {

class Win32FileReader : public FileReader {
public:
    Result read_file(
        const std::string& path,
        std::vector<char>& buffer_out,
        uint64_t& hash_out
    ) override {
        // Apri file con Win32 API per performance ottimali
        HANDLE hFile = CreateFileA(
            path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );
        
        if (hFile == INVALID_HANDLE_VALUE) {
            DWORD err = GetLastError();
            if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
                return Result::error(ErrorCode::FileNotFound, 
                    "File non trovato: " + path);
            }
            if (err == ERROR_ACCESS_DENIED) {
                return Result::error(ErrorCode::PermissionDenied,
                    "Accesso negato: " + path);
            }
            return Result::error(ErrorCode::CannotOpenFile,
                "Impossibile aprire: " + path + " (Errore: " + std::to_string(err) + ")");
        }
        
        // Ottieni dimensione file
        LARGE_INTEGER file_size;
        if (!GetFileSizeEx(hFile, &file_size)) {
            CloseHandle(hFile);
            return Result::error(ErrorCode::CannotReadFile,
                "Impossibile ottenere dimensione file: " + path);
        }
        
        if (file_size.QuadPart == 0) {
            CloseHandle(hFile);
            buffer_out.clear();
            hash_out = 0;
            return Result::success();
        }
        
        // Alloca buffer
        try {
            buffer_out.resize(file_size.QuadPart);
        } catch (const std::bad_alloc&) {
            CloseHandle(hFile);
            return Result::error(ErrorCode::OutOfMemory,
                "Memoria insufficiente per: " + path + " (" + 
                std::to_string(file_size.QuadPart) + " byte)");
        }
        
        // Lettura con hash incrementale
        XXH64_state_t* hash_state = XXH64_createState();
        if (!hash_state) {
            CloseHandle(hFile);
            return Result::error(ErrorCode::OutOfMemory, "Hash state allocation failed");
        }
        XXH64_reset(hash_state, 0);
        
        DWORD total_read = 0;
        char* ptr = buffer_out.data();
        
        while (total_read < file_size.QuadPart) {
            DWORD to_read = static_cast<DWORD>(
                std::min<uint64_t>(constants::IO_BUFFER_SIZE, 
                                   file_size.QuadPart - total_read)
            );
            
            DWORD bytes_read = 0;
            if (!ReadFile(hFile, ptr + total_read, to_read, &bytes_read, nullptr)) {
                XXH64_freeState(hash_state);
                CloseHandle(hFile);
                return Result::error(ErrorCode::CannotReadFile,
                    "Errore lettura: " + path + " (Errore: " + 
                    std::to_string(GetLastError()) + ")");
            }
            
            if (bytes_read == 0) break;  // EOF inatteso
            
            XXH64_update(hash_state, ptr + total_read, bytes_read);
            total_read += bytes_read;
        }
        
        hash_out = XXH64_digest(hash_state);
        XXH64_freeState(hash_state);
        CloseHandle(hFile);
        
        if (total_read != file_size.QuadPart) {
            return Result::error(ErrorCode::CannotReadFile,
                "Lettura incompleta: " + path);
        }
        
        return Result::success();
    }
    
    Result get_file_size(const std::string& path, uint64_t& size_out) override {
        HANDLE hFile = CreateFileA(
            path.c_str(), 0, FILE_SHARE_READ, nullptr, 
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr
        );
        
        if (hFile == INVALID_HANDLE_VALUE) {
            return Result::error(ErrorCode::FileNotFound, path);
        }
        
        LARGE_INTEGER size;
        bool success = GetFileSizeEx(hFile, &size);
        CloseHandle(hFile);
        
        if (!success) {
            return Result::error(ErrorCode::CannotReadFile, path);
        }
        
        size_out = size.QuadPart;
        return Result::success();
    }
    
    Result get_file_timestamp(const std::string& path, uint64_t& timestamp_out) override {
        try {
            auto ftime = std::filesystem::last_write_time(path);
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ftime - std::filesystem::file_time_type::clock::now() + 
                std::chrono::system_clock::now()
            );
            timestamp_out = std::chrono::system_clock::to_time_t(sctp);
            return Result::success();
        } catch (...) {
            return Result::error(ErrorCode::CannotReadFile, 
                "Impossibile leggere timestamp: " + path);
        }
    }
    
    bool file_exists(const std::string& path) const override {
        return std::filesystem::exists(path) && std::filesystem::is_regular_file(path);
    }
};

std::unique_ptr<FileReader> create_file_reader() {
    return std::make_unique<Win32FileReader>();
}

} // namespace tarc::io

#endif // _WIN32
