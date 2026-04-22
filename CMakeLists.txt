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
    add_compile_options(/DXXH_INLINE_ALL)
else()
    add_compile_options(-Wall -Wextra -O3 -flto)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64|AMD64|amd64)")
        add_compile_options(-march=native)
    endif()
endif()

include_directories(include)

# -----------------------------------------------------------------------------
# Scarica automaticamente i sorgenti se non presenti
# -----------------------------------------------------------------------------
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/libs/lzma/src/liblzma/common/common.c")
    message(STATUS "LZMA sources not found, downloading...")
    file(DOWNLOAD 
        "https://github.com/tukaani-project/xz/releases/download/v5.4.5/xz-5.4.5.tar.gz"
        "${CMAKE_BINARY_DIR}/lzma.tar.gz"
        STATUS download_status
        SHOW_PROGRESS
    )
    if(download_status EQUAL 0)
        execute_process(COMMAND ${CMAKE_COMMAND} -E tar xzf "${CMAKE_BINARY_DIR}/lzma.tar.gz" 
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
        file(MAKE_DIRECTORY "${CMAKE_SOURCE_DIR}/libs/lzma")
        execute_process(COMMAND ${CMAKE_COMMAND} -E copy_directory 
            "${CMAKE_BINARY_DIR}/xz-5.4.5" "${CMAKE_SOURCE_DIR}/libs/lzma")
        file(REMOVE "${CMAKE_BINARY_DIR}/lzma.tar.gz")
        message(STATUS "LZMA sources downloaded successfully")
    else()
        message(FATAL_ERROR "Failed to download LZMA sources")
    endif()
endif()

if(NOT EXISTS "${CMAKE_SOURCE_DIR}/libs/xxhash/xxhash.c")
    message(STATUS "XXHASH sources not found, downloading...")
    file(DOWNLOAD 
        "https://github.com/Cyan4973/xxHash/releases/download/v0.8.2/xxhash-0.8.2.tar.gz"
        "${CMAKE_BINARY_DIR}/xxhash.tar.gz"
        STATUS download_status
        SHOW_PROGRESS
    )
    if(download_status EQUAL 0)
        execute_process(COMMAND ${CMAKE_COMMAND} -E tar xzf "${CMAKE_BINARY_DIR}/xxhash.tar.gz" 
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
        file(MAKE_DIRECTORY "${CMAKE_SOURCE_DIR}/libs/xxhash")
        execute_process(COMMAND ${CMAKE_COMMAND} -E copy_directory 
            "${CMAKE_BINARY_DIR}/xxhash-0.8.2" "${CMAKE_SOURCE_DIR}/libs/xxhash")
        file(REMOVE "${CMAKE_BINARY_DIR}/xxhash.tar.gz")
        message(STATUS "XXHASH sources downloaded successfully")
    else()
        message(FATAL_ERROR "Failed to download XXHASH sources")
    endif()
endif()

# -----------------------------------------------------------------------------
# XXHASH - compilato dai sorgenti
# -----------------------------------------------------------------------------
set(XXHASH_SOURCES
    libs/xxhash/xxhash.c
)

add_library(xxhash_static STATIC ${XXHASH_SOURCES})
target_include_directories(xxhash_static PUBLIC libs/xxhash)

if(MSVC)
    target_compile_definitions(xxhash_static PRIVATE XXH_INLINE_ALL)
endif()

# -----------------------------------------------------------------------------
# LZMA - compilato dai sorgenti
# -----------------------------------------------------------------------------
file(GLOB_RECURSE LZMA_SOURCES 
    "libs/lzma/src/liblzma/*.c"
    "libs/lzma/src/common/*.c"
)

# Rimuovi file problematici o non necessari
list(REMOVE_ITEM LZMA_SOURCES 
    "libs/lzma/src/liblzma/check/crc32_arm64.c"
    "libs/lzma/src/liblzma/check/crc32_small.c"
    "libs/lzma/src/liblzma/check/crc64_small.c"
    "libs/lzma/src/liblzma/simple/arm64.c"
    "libs/lzma/src/liblzma/simple/riscv.c"
)

# Aggiungi i file header ai sorgenti (per permettere a CMake di trovarli)
file(GLOB_RECURSE LZMA_HEADERS
    "libs/lzma/src/liblzma/*.h"
    "libs/lzma/src/common/*.h"
)

add_library(lzma_static STATIC ${LZMA_SOURCES} ${LZMA_HEADERS})

# Include directories per LZMA
target_include_directories(lzma_static PUBLIC 
    libs/lzma/src/liblzma/api
    libs/lzma/src/liblzma
    libs/lzma/src/common
    libs/lzma/src/liblzma/check
    libs/lzma/src/liblzma/common
    libs/lzma/src/liblzma/lz
    libs/lzma/src/liblzma/lzma
    libs/lzma/src/liblzma/rangecoder
    libs/lzma/src/liblzma/simple
    libs/lzma/src/liblzma/delta
)

if(MSVC)
    target_compile_definitions(lzma_static PRIVATE 
        LZMA_API_STATIC
        _CRT_SECURE_NO_WARNINGS
    )
else()
    target_compile_definitions(lzma_static PRIVATE LZMA_API_STATIC)
endif()

# -----------------------------------------------------------------------------
# Sorgenti principali
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
    include
)

target_link_libraries(tarc
    xxhash_static
    lzma_static
)

if(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(tarc Threads::Threads)
endif()

install(TARGETS tarc RUNTIME DESTINATION bin)

message(STATUS "")
message(STATUS "========================================")
message(STATUS "TARC Strike v2.0")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "XXHASH: static (compiled from sources)")
message(STATUS "LZMA: static (compiled from sources)")
message(STATUS "========================================")
message(STATUS "")
