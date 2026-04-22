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
# XXHASH - compilato dai sorgenti (funziona su tutte le piattaforme)
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
# LZMA - cerca libreria di sistema
# -----------------------------------------------------------------------------
if(UNIX)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBLZMA REQUIRED liblzma)
    set(LZMA_INCLUDE_DIRS ${LIBLZMA_INCLUDE_DIRS})
    set(LZMA_LIBRARIES ${LIBLZMA_LIBRARIES})
else()
    # Windows
    find_path(LZMA_INCLUDE_DIR lzma.h
        PATHS "C:/vcpkg/installed/x64-windows/include"
              "C:/Program Files/liblzma/include"
              "$ENV{VCPKG_ROOT}/installed/x64-windows/include"
              ${CMAKE_SOURCE_DIR}/libs/lzma/src/liblzma/api
    )
    find_library(LZMA_LIBRARY lzma
        PATHS "C:/vcpkg/installed/x64-windows/lib"
              "C:/Program Files/liblzma/lib"
              "$ENV{VCPKG_ROOT}/installed/x64-windows/lib"
    )
    
    if(NOT LZMA_INCLUDE_DIR)
        # Usa i sorgenti inclusi se disponibili
        set(LZMA_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/libs/lzma/src/liblzma/api)
    endif()
    
    set(LZMA_INCLUDE_DIRS ${LZMA_INCLUDE_DIR})
    set(LZMA_LIBRARIES ${LZMA_LIBRARY})
endif()

if(NOT LZMA_INCLUDE_DIRS OR NOT LZMA_LIBRARIES)
    message(FATAL_ERROR "LZMA library not found. Install liblzma-dev (Linux), xz (macOS), or ensure libs/lzma exists on Windows")
endif()

# -----------------------------------------------------------------------------
# ZLIB per CRC32
# -----------------------------------------------------------------------------
if(UNIX)
    find_path(ZLIB_INCLUDE_DIR zlib.h
        PATHS /usr/include /usr/local/include /opt/homebrew/include
    )
    find_library(ZLIB_LIBRARY z
        PATHS /usr/lib /usr/local/lib /opt/homebrew/lib
    )
else()
    find_path(ZLIB_INCLUDE_DIR zlib.h
        PATHS "C:/vcpkg/installed/x64-windows/include"
              "C:/Program Files/zlib/include"
              "$ENV{VCPKG_ROOT}/installed/x64-windows/include"
    )
    find_library(ZLIB_LIBRARY zlib
        PATHS "C:/vcpkg/installed/x64-windows/lib"
              "C:/Program Files/zlib/lib"
              "$ENV{VCPKG_ROOT}/installed/x64-windows/lib"
    )
endif()

if(NOT ZLIB_INCLUDE_DIR OR NOT ZLIB_LIBRARY)
    message(FATAL_ERROR "ZLIB library not found. Install zlib1g-dev (Linux), zlib (macOS), or zlib (Windows)")
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
    ${LZMA_INCLUDE_DIRS}
    ${ZLIB_INCLUDE_DIR}
    include
)

target_link_libraries(tarc
    xxhash_static
    ${LZMA_LIBRARIES}
    ${ZLIB_LIBRARY}
)

find_package(Threads REQUIRED)
target_link_libraries(tarc Threads::Threads)

install(TARGETS tarc RUNTIME DESTINATION bin)

message(STATUS "")
message(STATUS "========================================")
message(STATUS "TARC Strike v2.0")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "XXHASH: static (compiled from sources)")
message(STATUS "LZMA: ${LZMA_LIBRARIES}")
message(STATUS "ZLIB: ${ZLIB_LIBRARY}")
message(STATUS "========================================")
message(STATUS "")
