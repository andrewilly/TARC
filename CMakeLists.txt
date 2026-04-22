cmake_minimum_required(VERSION 3.15)
project(TARC VERSION 2.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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
# Cerca LZMA (liblzma)
# -----------------------------------------------------------------------------
if(MSVC)
    # Windows: cerca in percorsi vcpkg e standard
    find_path(LZMA_INCLUDE_DIR lzma.h
        PATHS
            "C:/Program Files/liblzma/include"
            "C:/vcpkg/installed/x64-windows/include"
            "C:/vcpkg/installed/x86-windows/include"
            "$ENV{VCPKG_ROOT}/installed/x64-windows/include"
            "C:/msys64/mingw64/include"
            "C:/msys64/ucrt64/include"
            "C:/tools/msys64/mingw64/include"
    )
    find_library(LZMA_LIBRARY
        NAMES lzma liblzma
        PATHS
            "C:/Program Files/liblzma/lib"
            "C:/vcpkg/installed/x64-windows/lib"
            "C:/vcpkg/installed/x86-windows/lib"
            "$ENV{VCPKG_ROOT}/installed/x64-windows/lib"
            "C:/msys64/mingw64/lib"
            "C:/msys64/ucrt64/lib"
            "C:/tools/msys64/mingw64/lib"
    )
elseif(APPLE)
    # macOS: cerca con Homebrew
    find_path(LZMA_INCLUDE_DIR lzma.h
        PATHS
            /opt/homebrew/include
            /usr/local/include
            /usr/include
    )
    find_library(LZMA_LIBRARY
        NAMES lzma liblzma
        PATHS
            /opt/homebrew/lib
            /usr/local/lib
            /usr/lib
    )
else()
    # Linux: percorsi standard
    find_path(LZMA_INCLUDE_DIR lzma.h
        PATHS
            /usr/include
            /usr/local/include
    )
    find_library(LZMA_LIBRARY
        NAMES lzma liblzma
        PATHS
            /usr/lib
            /usr/local/lib
            /usr/lib/x86_64-linux-gnu
            /usr/lib/aarch64-linux-gnu
    )
endif()

if(NOT LZMA_INCLUDE_DIR OR NOT LZMA_LIBRARY)
    message(STATUS "LZMA library not found. Will use system default paths.")
    # Non fallire, alcune piattaforme potrebbero avere lzma in un percorso diverso
endif()

# -----------------------------------------------------------------------------
# Cerca XXHASH
# -----------------------------------------------------------------------------
if(MSVC)
    find_path(XXHASH_INCLUDE_DIR xxhash.h
        PATHS
            "C:/Program Files/xxhash/include"
            "C:/vcpkg/installed/x64-windows/include"
            "C:/vcpkg/installed/x86-windows/include"
            "$ENV{VCPKG_ROOT}/installed/x64-windows/include"
            "C:/msys64/mingw64/include"
            "C:/msys64/ucrt64/include"
    )
    find_library(XXHASH_LIBRARY
        NAMES xxhash libxxhash
        PATHS
            "C:/Program Files/xxhash/lib"
            "C:/vcpkg/installed/x64-windows/lib"
            "C:/vcpkg/installed/x86-windows/lib"
            "$ENV{VCPKG_ROOT}/installed/x64-windows/lib"
            "C:/msys64/mingw64/lib"
            "C:/msys64/ucrt64/lib"
    )
elseif(APPLE)
    find_path(XXHASH_INCLUDE_DIR xxhash.h
        PATHS
            /opt/homebrew/include
            /usr/local/include
            /usr/include
    )
    find_library(XXHASH_LIBRARY
        NAMES xxhash libxxhash
        PATHS
            /opt/homebrew/lib
            /usr/local/lib
            /usr/lib
    )
else()
    find_path(XXHASH_INCLUDE_DIR xxhash.h
        PATHS
            /usr/include
            /usr/local/include
    )
    find_library(XXHASH_LIBRARY
        NAMES xxhash libxxhash
        PATHS
            /usr/lib
            /usr/local/lib
            /usr/lib/x86_64-linux-gnu
            /usr/lib/aarch64-linux-gnu
    )
endif()

if(NOT XXHASH_INCLUDE_DIR OR NOT XXHASH_LIBRARY)
    message(STATUS "XXHASH library not found. Will try to use system paths.")
endif()

# -----------------------------------------------------------------------------
# Cerca anche nei percorsi standard di sistema (fallback)
# -----------------------------------------------------------------------------
if(NOT LZMA_INCLUDE_DIR OR NOT LZMA_LIBRARY)
    find_path(LZMA_INCLUDE_DIR lzma.h)
    find_library(LZMA_LIBRARY lzma)
endif()

if(NOT XXHASH_INCLUDE_DIR OR NOT XXHASH_LIBRARY)
    find_path(XXHASH_INCLUDE_DIR xxhash.h)
    find_library(XXHASH_LIBRARY xxhash)
endif()

# -----------------------------------------------------------------------------
# Threads (solo per Unix)
# -----------------------------------------------------------------------------
if(UNIX)
    find_package(Threads REQUIRED)
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
)

# -----------------------------------------------------------------------------
# Eseguibile
# -----------------------------------------------------------------------------
add_executable(tarc ${SOURCES})

target_include_directories(tarc PRIVATE
    ${LZMA_INCLUDE_DIR}
    ${XXHASH_INCLUDE_DIR}
    include
)

# Link librerie (solo se trovate)
if(LZMA_LIBRARY)
    target_link_libraries(tarc ${LZMA_LIBRARY})
    message(STATUS "Linking LZMA: ${LZMA_LIBRARY}")
endif()

if(XXHASH_LIBRARY)
    target_link_libraries(tarc ${XXHASH_LIBRARY})
    message(STATUS "Linking XXHASH: ${XXHASH_LIBRARY}")
endif()

if(UNIX AND NOT APPLE)
    target_link_libraries(tarc Threads::Threads)
endif()

# -----------------------------------------------------------------------------
# Installazione
# -----------------------------------------------------------------------------
install(TARGETS tarc RUNTIME DESTINATION bin)

# -----------------------------------------------------------------------------
# Riepilogo
# -----------------------------------------------------------------------------
message(STATUS "")
message(STATUS "========================================")
message(STATUS "TARC Strike v${PROJECT_VERSION}")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
if(LZMA_LIBRARY)
    message(STATUS "LZMA: ${LZMA_LIBRARY}")
else()
    message(STATUS "LZMA: NOT FOUND - using system defaults")
endif()
if(XXHASH_LIBRARY)
    message(STATUS "XXHASH: ${XXHASH_LIBRARY}")
else()
    message(STATUS "XXHASH: NOT FOUND - using system defaults")
endif()
message(STATUS "========================================")
message(STATUS "")
