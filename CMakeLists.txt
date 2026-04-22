cmake_minimum_required(VERSION 3.20)
project(tarc VERSION 2.03 LANGUAGES CXX C)

# STANDARD
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# BUILD TYPE
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# MULTI-THREADING SUPPORT
find_package(Threads REQUIRED)

# FETCH CONTENT
include(FetchContent)

# ── zstd ─────────────────────────────────────────────────────────────────────
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

# ── lz4 ──────────────────────────────────────────────────────────────────────
FetchContent_Declare(
    lz4
    GIT_REPOSITORY https://github.com/lz4/lz4.git
    GIT_TAG        v1.10.0
    SOURCE_SUBDIR  build/cmake
)
set(LZ4_BUILD_CLI    OFF CACHE BOOL "" FORCE)
set(LZ4_BUILD_LEGACY OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(lz4)

# ── xz/lzma ──────────────────────────────────────────────────────────────────
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
set(HAVE_LIBINTL_H       OFF CACHE BOOL "" FORCE)
set(HAVE_LIBINTL         OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(liblzma)

# ── brotli ───────────────────────────────────────────────────────────────────
FetchContent_Declare(
    brotli
    GIT_REPOSITORY https://github.com/google/brotli.git
    GIT_TAG        v1.1.0
)
set(BROTLI_BUNDLED_MODE ON CACHE BOOL "" FORCE)
set(BROTLI_DISABLE_TESTS ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(brotli)

# ── xxhash ───────────────────────────────────────────────────────────────────
FetchContent_Declare(
    xxhash
    GIT_REPOSITORY https://github.com/Cyan4973/xxHash.git
    GIT_TAG        v0.8.2
)
FetchContent_MakeAvailable(xxhash)
add_library(xxhash_lib STATIC ${xxhash_SOURCE_DIR}/xxhash.c)
target_include_directories(xxhash_lib PUBLIC ${xxhash_SOURCE_DIR})

# SORGENTI
set(TARC_SOURCES
    src/main.cpp
    src/ui.cpp
    src/license.cpp
    src/io.cpp
    src/engine.cpp
)

add_executable(tarc ${TARC_SOURCES})

target_include_directories(tarc PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${zstd_SOURCE_DIR}/lib
    ${lz4_SOURCE_DIR}/lib
    ${liblzma_SOURCE_DIR}/src/liblzma/api
    ${brotli_SOURCE_DIR}/c/include
    ${xxhash_SOURCE_DIR}
)

# LINK
target_link_libraries(tarc PRIVATE
    libzstd_static
    lz4_static
    liblzma
    brotlienc
    brotlidec
    brotlicommon
    xxhash_lib
    Threads::Threads
)

# FLAGS OTTIMIZZAZIONE
if(MSVC)
    target_compile_options(tarc PRIVATE /O2 /W4 /EHsc /utf-8)
else()
    target_compile_options(tarc PRIVATE -O3 -Wall -Wextra -Wpedantic
        $<$<CXX_COMPILER_ID:GNU>:-march=native>
        $<$<CXX_COMPILER_ID:Clang>:-march=native>
    )
endif()

# DEFINIZIONI SPECIFICHE PER PIATTAFORMA
if(WIN32)
    target_compile_definitions(tarc PRIVATE
        _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX _USE_MATH_DEFINES)
    target_link_libraries(tarc PRIVATE bcrypt user32)
endif()

if(APPLE)
    target_compile_definitions(tarc PRIVATE _DARWIN_C_SOURCE)
    target_compile_options(tarc PRIVATE -Wno-unused-variable -Wno-unused-parameter)
endif()

if(UNIX AND NOT APPLE)
    target_compile_definitions(tarc PRIVATE _GNU_SOURCE)
    target_link_libraries(tarc PRIVATE dl)
endif()

# STRIP RELEASE
if(CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT MSVC)
    target_link_options(tarc PRIVATE -s)
endif()

# INFO BUILD
message(STATUS "")
message(STATUS "  +==========================================+")
message(STATUS "  |       TARC v${PROJECT_VERSION} - BUILD CONFIG        |")
message(STATUS "  +==========================================+")
message(STATUS "  |  Build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "  |  Parallel   : Enabled (Multi-threaded)")
message(STATUS "  |  Codecs     : ZSTD + LZMA + LZ4 + BROTLI")
message(STATUS "  |  Checksum   : XXH64 Chunk Verification")
message(STATUS "  |  I/O        : 64-bit + Atomic Writes")
message(STATUS "  |  Security   : Path Traversal Protection")
message(STATUS "  |  Features   : -o dir, --exclude, -v, -r, ETA")
message(STATUS "  +==========================================+")
message(STATUS "")
