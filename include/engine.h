#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <optional>
#include "types.h"

namespace CodecSelector {
    Codec select(const std::string& path, size_t size);
    bool is_compressible(const std::string& ext);
    void init();
}

class ProgressCallback {
public:
    virtual ~ProgressCallback() = default;
    virtual void on_progress(size_t current, size_t total, const std::string& current_file) = 0;
    virtual void on_warning(const std::string& msg) = 0;
    virtual bool is_cancelled() const = 0;
};

namespace Engine {

    struct CompressionStats {
        uint64_t files_processed = 0;
        uint64_t bytes_read = 0;
        uint64_t bytes_written = 0;
        uint64_t bytes_compressed = 0;
        uint64_t duplicates_skipped = 0;
        std::chrono::milliseconds elapsed{};
    };

    struct FileEntryInternal {
        std::string name;
        std::string extension;
    };

    TarcResult compress(const std::string& arch_path, const std::vector<std::string>& files, 
                       bool append = false, int level = 3);
    
    TarcResult extract(const std::string& arch_path, const std::vector<std::string>& patterns = {},
                    bool test_only = false, size_t offset = 0, bool flat_mode = false);
    
    TarcResult list(const std::string& arch_path, size_t offset = 0);
    
    TarcResult remove_files(const std::string& arch_path, const std::vector<std::string>& patterns);
    
    TarcResult create_sfx(const std::string& archive_path, const std::string& sfx_path);

    void set_progress_callback(ProgressCallback* callback);

    CompressionStats get_stats();
    
    void reset_stats();

}
