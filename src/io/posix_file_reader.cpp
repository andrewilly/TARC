#include "tarc/io/file_reader.h"
#include "tarc/security/checksum.h"
#include "tarc/util/constants.h"
#include <filesystem>
#include <fstream>
#include <cstring>

#ifndef _WIN32

extern "C" {
    #include "xxhash.h"
}

namespace tarc::io {

class PosixFileReader : public FileReader {
public:
    Result read_file(
        const std::string& path,
        std::vector<char>& buffer_out,
        uint64_t& hash_out
    ) override {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) {
            if (!std::filesystem::exists(path)) {
                return Result::error(ErrorCode::FileNotFound, 
                    "File non trovato: " + path);
            }
            return Result::error(ErrorCode::CannotOpenFile,
                "Impossibile aprire: " + path);
        }
        
        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);
        
        if (size == 0) {
            buffer_out.clear();
            hash_out = 0;
            return Result::success();
        }
        
        // Alloca buffer
        try {
            buffer_out.resize(size);
        } catch (const std::bad_alloc&) {
            return Result::error(ErrorCode::OutOfMemory,
                "Memoria insufficiente per: " + path + " (" + 
                std::to_string(size) + " byte)");
        }
        
        // Lettura con hash incrementale
        XXH64_state_t* hash_state = XXH64_createState();
        if (!hash_state) {
            return Result::error(ErrorCode::OutOfMemory, "Hash state allocation failed");
        }
        XXH64_reset(hash_state, 0);
        
        std::streamsize total_read = 0;
        while (total_read < size && file) {
            std::streamsize to_read = std::min<std::streamsize>(
                constants::IO_BUFFER_SIZE, 
                size - total_read
            );
            
            file.read(buffer_out.data() + total_read, to_read);
            std::streamsize bytes_read = file.gcount();
            
            if (bytes_read > 0) {
                XXH64_update(hash_state, buffer_out.data() + total_read, bytes_read);
                total_read += bytes_read;
            }
        }
        
        hash_out = XXH64_digest(hash_state);
        XXH64_freeState(hash_state);
        
        if (total_read != size) {
            return Result::error(ErrorCode::CannotReadFile,
                "Lettura incompleta: " + path);
        }
        
        return Result::success();
    }
    
    Result get_file_size(const std::string& path, uint64_t& size_out) override {
        try {
            size_out = std::filesystem::file_size(path);
            return Result::success();
        } catch (const std::filesystem::filesystem_error&) {
            return Result::error(ErrorCode::FileNotFound, path);
        }
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
    return std::make_unique<PosixFileReader>();
}

} // namespace tarc::io

#endif // !_WIN32
