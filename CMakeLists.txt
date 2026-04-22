cmake_minimum_required(VERSION 3.20)
project(tarc VERSION 2.00 LANGUAGES CXX C)

# ═══════════════════════════════════════════════════════════════════════════
# STANDARD C++20
# ═══════════════════════════════════════════════════════════════════════════

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ═══════════════════════════════════════════════════════════════════════════
# BUILD TYPE
# ═══════════════════════════════════════════════════════════════════════════

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ═══════════════════════════════════════════════════════════════════════════
# DEPENDENCIES
# ═══════════════════════════════════════════════════════════════════════════

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)  # Per HMAC-SHA256 nella licenza

include(FetchContent)

# ── ZSTD ──────────────────────────────────────────────────────────────────
FetchContent_Declare(
    zstd
    GIT_REPOSITORY https://github.com/facebook/zstd.git
    GIT_TAG        v1.5.6
    SOURCE_SUBDIR  build/cmake
)
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC   ON  CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED   OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(zstd)

# ── LZ4 ───────────────────────────────────────────────────────────────────
FetchContent_Declare(
    lz4
    GIT_REPOSITORY https://github.com/lz4/lz4.git
    GIT_TAG        v1.10.0
    SOURCE_SUBDIR  build/cmake
)
set(LZ4_BUILD_CLI    OFF CACHE BOOL "" FORCE)
set(LZ4_BUILD_LEGACY OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(lz4)

# ── LZMA/XZ ───────────────────────────────────────────────────────────────
FetchContent_Declare(
    liblzma
    GIT_REPOSITORY https://github.com/tukaani-project/xz.git
    GIT_TAG        v5.6.3
)
set(BUILD_SHARED_LIBS    OFF CACHE BOOL "" FORCE)
set(XZ_BUILD_EXECUTABLES OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_REQUIREMENTS OFF CACHE BOOL "" FORCE)
set(ENABLE_NLS           OFF CACHE BOOL "" FORCE)
set(DISABLE_NLS          ON  CACHE BOOL "" FORCE)
set(ENABLE_SANDBOX       OFF CACHE BOOL "" FORCE)
set(ENABLE_DOCS          OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(liblzma)

# ── BROTLI ────────────────────────────────────────────────────────────────
FetchContent_Declare(
    brotli
    GIT_REPOSITORY https://github.com/google/brotli.git
    GIT_TAG        v1.1.0
)
set(BROTLI_BUNDLED_MODE ON CACHE BOOL "" FORCE)
set(BROTLI_DISABLE_TESTS ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(brotli)

# ── XXHASH ────────────────────────────────────────────────────────────────
FetchContent_Declare(
    xxhash
    GIT_REPOSITORY https://github.com/Cyan4973/xxHash.git
    GIT_TAG        v0.8.2
)
FetchContent_MakeAvailable(xxhash)
add_library(xxhash_lib STATIC ${xxhash_SOURCE_DIR}/xxhash.c)
target_include_directories(xxhash_lib PUBLIC ${xxhash_SOURCE_DIR})

# ═══════════════════════════════════════════════════════════════════════════
# SOURCE FILES (MODULAR STRUCTURE)
# ═══════════════════════════════════════════════════════════════════════════

set(TARC_SOURCES
    # Main
    src/main.cpp
    
    # UI
    src/ui/ui.cpp
    
    # Security
    src/security/validator.cpp
    src/security/checksum.cpp
    src/security/license.cpp
    
    # I/O
    src/io/file_writer.cpp
    src/io/toc_manager.cpp
    src/io/chunk_stream.cpp
    
    # Codec
    src/codec/codec_selector.cpp
    
    # Core (placeholder - da completare)
    # src/core/compressor.cpp
    # src/core/extractor.cpp
)

# Platform-specific file readers
if(WIN32)
    list(APPEND TARC_SOURCES src/io/win32_file_reader.cpp)
else()
    list(APPEND TARC_SOURCES src/io/posix_file_reader.cpp)
endif()

# ═══════════════════════════════════════════════════════════════════════════
# EXECUTABLE
# ═══════════════════════════════════════════════════════════════════════════

add_executable(tarc ${TARC_SOURCES})

target_include_directories(tarc PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${zstd_SOURCE_DIR}/lib
    ${lz4_SOURCE_DIR}/lib
    ${liblzma_SOURCE_DIR}/src/liblzma/api
    ${brotli_SOURCE_DIR}/c/include
    ${xxhash_SOURCE_DIR}
)

# ═══════════════════════════════════════════════════════════════════════════
# LINKING
# ═══════════════════════════════════════════════════════════════════════════

target_link_libraries(tarc PRIVATE
    libzstd_static
    lz4_static
    liblzma
    brotlienc
    brotlidec
    brotlicommon
    xxhash_lib
    OpenSSL::Crypto
    Threads::Threads
)

# ═══════════════════════════════════════════════════════════════════════════
# COMPILER FLAGS
# ═══════════════════════════════════════════════════════════════════════════

if(MSVC)
    target_compile_options(tarc PRIVATE 
        /W4 /O2 /EHsc /utf-8
        /D_CRT_SECURE_NO_WARNINGS
        /DWIN32_LEAN_AND_MEAN
        /DNOMINMAX
    )
    target_link_libraries(tarc PRIVATE bcrypt user32)
else()
    target_compile_options(tarc PRIVATE 
        -O3 -Wall -Wextra -Wpedantic
        -march=native
    )
endif()

# Platform definitions
if(WIN32)
    target_compile_definitions(tarc PRIVATE 
        _CRT_SECURE_NO_WARNINGS 
        WIN32_LEAN_AND_MEAN 
        NOMINMAX
    )
endif()

if(UNIX AND NOT APPLE)
    target_compile_definitions(tarc PRIVATE _GNU_SOURCE)
    target_link_libraries(tarc PRIVATE dl)
endif()

if(APPLE)
    target_compile_definitions(tarc PRIVATE _DARWIN_C_SOURCE)
endif()

# Strip in release
if(CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT MSVC)
    target_link_options(tarc PRIVATE -s)
endif()

# ═══════════════════════════════════════════════════════════════════════════
# INFO
# ═══════════════════════════════════════════════════════════════════════════

message(STATUS "")
message(STATUS "  ╔════════════════════════════════════════════╗")
message(STATUS "  ║   TARC v${PROJECT_VERSION} - REFACTORED BUILD   ║")
message(STATUS "  ╠════════════════════════════════════════════╣")
message(STATUS "  ║  C++ Standard : C++20")
message(STATUS "  ║  Build Type   : ${CMAKE_BUILD_TYPE}")
message(STATUS "  ║  Platform     : ${CMAKE_SYSTEM_NAME}")
message(STATUS "  ║  Compiler     : ${CMAKE_CXX_COMPILER_ID}")
message(STATUS "  ║  Security     : HMAC-SHA256 + Path Validation")
message(STATUS "  ║  Architecture : Modular (10+ modules)")
message(STATUS "  ╚════════════════════════════════════════════╝")
message(STATUS "")
