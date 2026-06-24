cmake_minimum_required(VERSION 3.20)
project(tarc VERSION 2.10 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Threads REQUIRED)

include(FetchContent)

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

FetchContent_Declare(
    lz4
    GIT_REPOSITORY https://github.com/lz4/lz4.git
    GIT_TAG        v1.10.0
    SOURCE_SUBDIR  build/cmake
)
set(LZ4_BUILD_CLI    OFF CACHE BOOL "" FORCE)
set(LZ4_BUILD_LEGACY OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(lz4)

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

FetchContent_Declare(
    brotli
    GIT_REPOSITORY https://github.com/google/brotli.git
    GIT_TAG        v1.1.0
)
set(BROTLI_BUNDLED_MODE ON CACHE BOOL "" FORCE)
set(BROTLI_DISABLE_TESTS ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(brotli)

FetchContent_Declare(
    xxhash
    GIT_REPOSITORY https://github.com/Cyan4973/xxHash.git
    GIT_TAG        v0.8.2
)
FetchContent_MakeAvailable(xxhash)
add_library(xxhash_lib STATIC ${xxhash_SOURCE_DIR}/xxhash.c)
target_include_directories(xxhash_lib PUBLIC ${xxhash_SOURCE_DIR})

# ============================================================================
# Sorgenti — il core e' condiviso tra tarc e tarc_sfx_stub
# ============================================================================
set(TARC_CORE_SOURCES
    src/ui.cpp
    src/io.cpp
    src/engine.cpp
)

set(TARC_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${zstd_SOURCE_DIR}/lib
    ${lz4_SOURCE_DIR}/lib
    ${liblzma_SOURCE_DIR}/src/liblzma/api
    ${brotli_SOURCE_DIR}/c/include
    ${xxhash_SOURCE_DIR}
)

set(TARC_LINK_LIBS
    libzstd_static
    lz4_static
    liblzma
    brotlienc
    brotlidec
    brotlicommon
    xxhash_lib
    Threads::Threads
)

# ============================================================================
# Icona Windows — solo su WIN32, usa il resource compiler (rc.exe / windres)
# ============================================================================
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/res/tarc.ico")
    set(TARC_ICON_RC "${CMAKE_CURRENT_SOURCE_DIR}/res/tarc.rc")
    message(STATUS "  Icona Windows: res/tarc.ico trovata, inclusa nel build")
else()
    set(TARC_ICON_RC "")
    if(WIN32)
        message(STATUS "  Icona Windows: res/tarc.ico NON trovata, exe senza icona")
    endif()
endif()

# ============================================================================
# Target 1: tarc — Tool principale
# ============================================================================
set(TARC_SOURCES
    src/main.cpp
    ${TARC_CORE_SOURCES}
)

if(TARC_ICON_RC)
    add_executable(tarc ${TARC_SOURCES} ${TARC_ICON_RC})
    # Il .rc include windows.h — serve il percorso della cartella res/
    target_include_directories(tarc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/res)
else()
    add_executable(tarc ${TARC_SOURCES})
endif()

target_include_directories(tarc PRIVATE ${TARC_INCLUDE_DIRS})

# Platform-specific definitions per il target principale tarc
if(WIN32)
    target_compile_definitions(tarc PRIVATE
        _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX _USE_MATH_DEFINES)
    if(MSVC)
        target_compile_options(tarc PRIVATE /O2 /W4 /EHsc /utf-8 /permissive-)
    endif()
endif()

if(APPLE)
    target_compile_definitions(tarc PRIVATE _DARWIN_C_SOURCE)
    target_compile_options(tarc PRIVATE -Wno-unused-variable -Wno-unused-parameter)
endif()

if(UNIX AND NOT APPLE)
    target_compile_definitions(tarc PRIVATE _GNU_SOURCE)
endif()

if(MSVC)
    set(ZSTD_FOUND FALSE)
    message(WARNING "ZSTD/LZ4 support is experimental on MSVC. Using LZMA as primary codec.")
else()
    if(TARGET libzstd_static)
        set(ZSTD_FOUND TRUE)
    endif()
endif()

if(ZSTD_FOUND)
    target_compile_definitions(tarc PRIVATE HAVE_ZSTD=1)
endif()

target_link_libraries(tarc PRIVATE ${TARC_LINK_LIBS})

# ============================================================================
# Target 2: tarc_sfx_stub — Stub autoestraente per archivi SFX
# ============================================================================
# Condivide engine/io/ui/license con tarc, ma ha un main() separato in sfx_stub.cpp
# che implementa la logica SFX (legge se stesso, trova archivio embeddato, estrae).
#
# Uso:
#   tarc -c9 archivio *.cpp --sfx
#
# Il flag TARC_SFX_STUB e' definito per il stub (per futuri #ifdef se necessari).
# ============================================================================

option(TARC_BUILD_SFX "Build tarc_sfx_stub for self-extracting archives" ON)

if(TARC_BUILD_SFX)
    set(SFX_SOURCES
        src/sfx_stub.cpp
        ${TARC_CORE_SOURCES}
    )

    add_executable(tarc_sfx_stub ${SFX_SOURCES})

    target_include_directories(tarc_sfx_stub PRIVATE ${TARC_INCLUDE_DIRS})

    # Stesse definizioni e flags del target principale
    if(ZSTD_FOUND)
        target_compile_definitions(tarc_sfx_stub PRIVATE HAVE_ZSTD=1)
    endif()

    target_compile_definitions(tarc_sfx_stub PRIVATE TARC_SFX_STUB=1)

    target_link_libraries(tarc_sfx_stub PRIVATE ${TARC_LINK_LIBS})

    # Stesse opzioni di compilazione per coerenza
    if(MSVC)
        target_compile_options(tarc_sfx_stub PRIVATE /O2 /W4 /EHsc /utf-8 /permissive-)
    else()
        target_compile_options(tarc_sfx_stub PRIVATE -O3 -Wall -Wextra -Wpedantic -Wno-unused-parameter
            $<$<CXX_COMPILER_ID:GNU>:-march=native>
            $<$<CXX_COMPILER_ID:Clang>:-march=native>
        )
    endif()

    if(WIN32)
        target_compile_definitions(tarc_sfx_stub PRIVATE
            _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX _USE_MATH_DEFINES)
        target_link_libraries(tarc_sfx_stub PRIVATE bcrypt user32)
    endif()

    if(APPLE)
        target_compile_definitions(tarc_sfx_stub PRIVATE _DARWIN_C_SOURCE)
        target_compile_options(tarc_sfx_stub PRIVATE -Wno-unused-variable -Wno-unused-parameter)
    endif()

    if(UNIX AND NOT APPLE)
        target_compile_definitions(tarc_sfx_stub PRIVATE _GNU_SOURCE)
        target_link_libraries(tarc_sfx_stub PRIVATE dl)
    endif()

    if(CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT MSVC)
        target_link_options(tarc_sfx_stub PRIVATE -s)
    endif()

    # Copia lo stub nella build root per accesso facile
    add_custom_command(TARGET tarc_sfx_stub POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:tarc_sfx_stub>
            ${CMAKE_BINARY_DIR}/tarc_sfx_stub${CMAKE_EXECUTABLE_SUFFIX}
        COMMENT "Copying SFX stub to build directory"
    )
endif()

# ============================================================================
# Target 3: tarc_test — Test suite (doctest)
# ============================================================================
option(TARC_BUILD_TESTS "Build tarc_test executable" ON)

if(TARC_BUILD_TESTS)
    # doctest header-only — gia' presente in include/doctest/doctest.h
    set(TEST_SOURCES
        test/tests.cpp
        ${TARC_CORE_SOURCES}
    )

    add_executable(tarc_test ${TEST_SOURCES})

    target_include_directories(tarc_test PRIVATE ${TARC_INCLUDE_DIRS})

    target_link_libraries(tarc_test PRIVATE ${TARC_LINK_LIBS})

    if(WIN32)
        target_compile_definitions(tarc_test PRIVATE
            _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX _USE_MATH_DEFINES)
    endif()
    if(APPLE)
        target_compile_definitions(tarc_test PRIVATE _DARWIN_C_SOURCE)
        target_compile_options(tarc_test PRIVATE -Wno-unused-variable -Wno-unused-parameter)
    endif()
    if(UNIX AND NOT APPLE)
        target_compile_definitions(tarc_test PRIVATE _GNU_SOURCE)
    endif()
    if(ZSTD_FOUND)
        target_compile_definitions(tarc_test PRIVATE HAVE_ZSTD=1)
    endif()

    add_custom_command(TARGET tarc_test POST_BUILD
        COMMAND $<TARGET_FILE:tarc_test>
        COMMENT "Running TARC test suite..."
    )
endif()

# ============================================================================
# Install targets
# ============================================================================
install(TARGETS tarc RUNTIME DESTINATION bin)
if(TARC_BUILD_SFX)
    install(TARGETS tarc_sfx_stub RUNTIME DESTINATION bin)
endif()

message(STATUS "")
message(STATUS "  ╔════════════════════════════════════════════╗")
message(STATUS "  ║         TARC v2.10 - BUILD CONFIG  ║")
message(STATUS "  ╠════════════════════════════════════════════╣")
message(STATUS "  ║  Build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "  ║  Standard   : C++17")
message(STATUS "  ║  Parallel   : Enabled (Multi-threaded)")
message(STATUS "  ║  Codecs     : LZMA + ZSTD + LZ4 + BROTLI")
message(STATUS "  ║  SFX Stub   : ${TARC_BUILD_SFX}")
message(STATUS "  ║  Version    : 2.10")
message(STATUS "  ╚════════════════════════════════════════════╝")
message(STATUS "")
