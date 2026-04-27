cmake_minimum_required(VERSION 3.20)
project(tarc VERSION 1.03 LANGUAGES CXX C)

# ─── STANDARD ────────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ─── BUILD TYPE ──────────────────────────────────────────────────────────────
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ─── FETCH CONTENT ───────────────────────────────────────────────────────────
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

# Disabilita TUTTO ciò che non serve (solo la libreria statica)
set(BUILD_SHARED_LIBS    OFF CACHE BOOL "" FORCE)
set(XZ_BUILD_EXECUTABLES OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_REQUIREMENTS OFF CACHE BOOL "" FORCE)
set(ENABLE_NLS           OFF CACHE BOOL "" FORCE)
set(DISABLE_NLS          ON  CACHE BOOL "" FORCE)
set(ENABLE_SANDBOX       OFF CACHE BOOL "" FORCE)
set(ENABLE_DOCS          OFF CACHE BOOL "" FORCE)

# Forza la disabilitazione di gettext/libintl
set(HAVE_LIBINTL_H       OFF CACHE BOOL "" FORCE)
set(HAVE_LIBINTL         OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(liblzma)

# Su macOS, escludi esplicitamente i target degli eseguibili
if(APPLE)
    foreach(target IN ITEMS lzmainfo xzdec lzmadec xz)
        if(TARGET ${target})
            set_target_properties(${target} PROPERTIES 
                EXCLUDE_FROM_ALL TRUE
                EXCLUDE_FROM_DEFAULT_BUILD TRUE
            )
        endif()
    endforeach()
endif()

# ── xxhash ───────────────────────────────────────────────────────────────────
FetchContent_Declare(
    xxhash
    GIT_REPOSITORY https://github.com/Cyan4973/xxHash.git
    GIT_TAG        v0.8.2
)
FetchContent_MakeAvailable(xxhash)

# Xxhash non ha un CMakeLists root-level per la lib, creiamo target manuale
add_library(xxhash_lib STATIC ${xxhash_SOURCE_DIR}/xxhash.c)
target_include_directories(xxhash_lib PUBLIC ${xxhash_SOURCE_DIR})

# ─── SORGENTI ────────────────────────────────────────────────────────────────
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
    ${xxhash_SOURCE_DIR}
)

# ─── LINK ────────────────────────────────────────────────────────────────────
target_link_libraries(tarc PRIVATE
    libzstd_static
    lz4_static
    liblzma
    xxhash_lib
)

# ─── FLAGS OTTIMIZZAZIONE ────────────────────────────────────────────────────
if(MSVC)
    target_compile_options(tarc PRIVATE 
        /O2 
        /W4 
        /EHsc
        /utf-8
    )
else()
    target_compile_options(tarc PRIVATE
        -O3
        -Wall
        -Wextra
        -Wpedantic
        $<$<CXX_COMPILER_ID:GNU>:-march=native>
        $<$<CXX_COMPILER_ID:Clang>:-march=native>
    )
endif()

# ─── DEFINIZIONI SPECIFICHE PER PIATTAFORMA ──────────────────────────────────

# Windows
if(WIN32)
    target_compile_definitions(tarc PRIVATE 
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _USE_MATH_DEFINES
    )
    target_link_libraries(tarc PRIVATE
        bcrypt
        user32
    )
endif()

# macOS
if(APPLE)
    target_compile_definitions(tarc PRIVATE 
        _DARWIN_C_SOURCE
    )
    # Disabilita warning per variabili non utilizzate
    target_compile_options(tarc PRIVATE 
        -Wno-unused-variable
        -Wno-unused-parameter
    )
endif()

# Linux
if(UNIX AND NOT APPLE)
    target_compile_definitions(tarc PRIVATE
        _GNU_SOURCE
    )
    target_link_libraries(tarc PRIVATE
        pthread
        dl
    )
endif()

# ─── STRIP RELEASE ───────────────────────────────────────────────────────────
if(CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT MSVC)
    target_link_options(tarc PRIVATE -s)
endif()

# ─── INFO ────────────────────────────────────────────────────────────────────
message(STATUS "")
message(STATUS "  ╔════════════════════════════════════════════╗")
message(STATUS "  ║         TARC v${PROJECT_VERSION} - BUILD CONFIG       ║")
message(STATUS "  ╠════════════════════════════════════════════╣")
message(STATUS "  ║  Build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "  ║  Compiler   : ${CMAKE_CXX_COMPILER_ID}")
message(STATUS "  ║  C++ std    : C++${CMAKE_CXX_STANDARD}")
message(STATUS "  ║  Codecs     : ZSTD + LZ4 + LZMA")
message(STATUS "  ║  Database   : .mdb/.accdb optimized")
if(APPLE)
    message(STATUS "  ║  Platform   : macOS (Universal)")
elseif(WIN32)
    message(STATUS "  ║  Platform   : Windows")
else()
    message(STATUS "  ║  Platform   : Linux")
endif()
message(STATUS "  ╚════════════════════════════════════════════╝")
message(STATUS "")
