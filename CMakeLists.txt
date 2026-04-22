cmake_minimum_required(VERSION 3.15)
project(TARC VERSION 2.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Opzioni di compilazione
if(MSVC)
    add_compile_options(/W4 /O2 /MT)
    add_compile_options(/D_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -O3)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64|AMD64|amd64)")
        add_compile_options(-march=native)
    endif()
endif()

include_directories(include)

# -----------------------------------------------------------------------------
# Cerca LZMA (usa pkg-config su Unix, percorsi diretti su Windows)
# -----------------------------------------------------------------------------
if(UNIX)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBLZMA REQUIRED liblzma)
    set(LZMA_INCLUDE_DIRS ${LIBLZMA_INCLUDE_DIRS})
    set(LZMA_LIBRARIES ${LIBLZMA_LIBRARIES})
else()
    find_path(LZMA_INCLUDE_DIR lzma.h
        PATHS "C:/vcpkg/installed/x64-windows/include"
              "C:/Program Files/liblzma/include"
              "$ENV{VCPKG_ROOT}/installed/x64-windows/include"
    )
    find_library(LZMA_LIBRARY lzma
        PATHS "C:/vcpkg/installed/x64-windows/lib"
              "C:/Program Files/liblzma/lib"
              "$ENV{VCPKG_ROOT}/installed/x64-windows/lib"
    )
    set(LZMA_INCLUDE_DIRS ${LZMA_INCLUDE_DIR})
    set(LZMA_LIBRARIES ${LZMA_LIBRARY})
endif()

if(NOT LZMA_INCLUDE_DIRS OR NOT LZMA_LIBRARIES)
    message(FATAL_ERROR "LZMA library not found. Install liblzma-dev (Linux), xz (macOS), or liblzma (Windows)")
endif()

# -----------------------------------------------------------------------------
# Cerca XXHASH (senza pkg-config per compatibilità cross-platform)
# -----------------------------------------------------------------------------
if(UNIX)
    # Cerca in percorsi standard Unix
    find_path(XXHASH_INCLUDE_DIR xxhash.h
        PATHS /usr/include /usr/local/include /opt/homebrew/include
    )
    find_library(XXHASH_LIBRARY
        NAMES xxhash libxxhash
        PATHS /usr/lib /usr/local/lib /opt/homebrew/lib
    )
else()
    # Windows
    find_path(XXHASH_INCLUDE_DIR xxhash.h
        PATHS "C:/vcpkg/installed/x64-windows/include"
              "C:/Program Files/xxhash/include"
              "$ENV{VCPKG_ROOT}/installed/x64-windows/include"
    )
    find_library(XXHASH_LIBRARY
        NAMES xxhash libxxhash
        PATHS "C:/vcpkg/installed/x64-windows/lib"
              "C:/Program Files/xxhash/lib"
              "$ENV{VCPKG_ROOT}/installed/x64-windows/lib"
    )
endif()

if(NOT XXHASH_INCLUDE_DIR OR NOT XXHASH_LIBRARY)
    message(FATAL_ERROR "XXHASH library not found. Install libxxhash-dev (Linux), xxhash (macOS), or xxhash (Windows)")
endif()

# -----------------------------------------------------------------------------
# Cerca ZLIB (per CRC32)
# -----------------------------------------------------------------------------
if(UNIX)
    find_path(ZLIB_INCLUDE_DIR zlib.h
        PATHS /usr/include /usr/local/include /opt/homebrew/include
    )
    find_library(ZLIB_LIBRARY
        NAMES z
        PATHS /usr/lib /usr/local/lib /opt/homebrew/lib
    )
else()
    find_path(ZLIB_INCLUDE_DIR zlib.h
        PATHS "C:/vcpkg/installed/x64-windows/include"
              "C:/Program Files/zlib/include"
              "$ENV{VCPKG_ROOT}/installed/x64-windows/include"
    )
    find_library(ZLIB_LIBRARY
        NAMES zlib z
        PATHS "C:/vcpkg/installed/x64-windows/lib"
              "C:/Program Files/zlib/lib"
              "$ENV{VCPKG_ROOT}/installed/x64-windows/lib"
    )
endif()

if(NOT ZLIB_INCLUDE_DIR OR NOT ZLIB_LIBRARY)
    message(FATAL_ERROR "ZLIB library not found. Install zlib1g-dev (Linux), zlib (macOS), or zlib (Windows)")
endif()

# -----------------------------------------------------------------------------
# Sorgenti
# -----------------------------------------------------------------------------
set(SOURCES
    src/main.cpp
    src/engine.cpp
    src/io.cpp
    src/ui.cpp
    src/license.cpp
    src/error.cpp
)

add_executable(tarc ${SOURCES})

target_include_directories(tarc PRIVATE
    ${LZMA_INCLUDE_DIRS}
    ${XXHASH_INCLUDE_DIR}
    ${ZLIB_INCLUDE_DIR}
    include
)

target_link_libraries(tarc
    ${LZMA_LIBRARIES}
    ${XXHASH_LIBRARY}
    ${ZLIB_LIBRARY}
)

find_package(Threads REQUIRED)
target_link_libraries(tarc Threads::Threads)

install(TARGETS tarc RUNTIME DESTINATION bin)

message(STATUS "")
message(STATUS "========================================")
message(STATUS "TARC Strike v2.0")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "LZMA: ${LZMA_LIBRARIES}")
message(STATUS "XXHASH: ${XXHASH_LIBRARY}")
message(STATUS "ZLIB: ${ZLIB_LIBRARY}")
message(STATUS "========================================")
message(STATUS "")
