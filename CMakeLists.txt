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
# LZMA - compilato dai sorgenti (per tutte le piattaforme)
# -----------------------------------------------------------------------------
set(LZMA_SOURCES
    libs/lzma/src/liblzma/check/check.c
    libs/lzma/src/liblzma/check/crc32_fast.c
    libs/lzma/src/liblzma/check/crc32_table.c
    libs/lzma/src/liblzma/check/crc64_fast.c
    libs/lzma/src/liblzma/check/crc64_table.c
    libs/lzma/src/liblzma/check/sha256.c
    libs/lzma/src/liblzma/common/alone_decoder.c
    libs/lzma/src/liblzma/common/alone_encoder.c
    libs/lzma/src/liblzma/common/auto_decoder.c
    libs/lzma/src/liblzma/common/block_buffer_decoder.c
    libs/lzma/src/liblzma/common/block_buffer_encoder.c
    libs/lzma/src/liblzma/common/block_decoder.c
    libs/lzma/src/liblzma/common/block_encoder.c
    libs/lzma/src/liblzma/common/block_header_decoder.c
    libs/lzma/src/liblzma/common/block_header_encoder.c
    libs/lzma/src/liblzma/common/block_util.c
    libs/lzma/src/liblzma/common/common.c
    libs/lzma/src/liblzma/common/easy_buffer_encoder.c
    libs/lzma/src/liblzma/common/easy_decoder_memusage.c
    libs/lzma/src/liblzma/common/easy_encoder.c
    libs/lzma/src/liblzma/common/easy_encoder_memusage.c
    libs/lzma/src/liblzma/common/easy_preset.c
    libs/lzma/src/liblzma/common/filter_buffer_decoder.c
    libs/lzma/src/liblzma/common/filter_buffer_encoder.c
    libs/lzma/src/liblzma/common/filter_common.c
    libs/lzma/src/liblzma/common/filter_decoder.c
    libs/lzma/src/liblzma/common/filter_encoder.c
    libs/lzma/src/liblzma/common/filter_flags_decoder.c
    libs/lzma/src/liblzma/common/filter_flags_encoder.c
    libs/lzma/src/liblzma/common/hardware_cputhreads.c
    libs/lzma/src/liblzma/common/hardware_physmem.c
    libs/lzma/src/liblzma/common/index.c
    libs/lzma/src/liblzma/common/index_decoder.c
    libs/lzma/src/liblzma/common/index_encoder.c
    libs/lzma/src/liblzma/common/index_hash.c
    libs/lzma/src/liblzma/common/microlzma_decoder.c
    libs/lzma/src/liblzma/common/microlzma_encoder.c
    libs/lzma/src/liblzma/common/outqueue.c
    libs/lzma/src/liblzma/common/stream_buffer_decoder.c
    libs/lzma/src/liblzma/common/stream_buffer_encoder.c
    libs/lzma/src/liblzma/common/stream_decoder.c
    libs/lzma/src/liblzma/common/stream_decoder_mt.c
    libs/lzma/src/liblzma/common/stream_encoder.c
    libs/lzma/src/liblzma/common/stream_encoder_mt.c
    libs/lzma/src/liblzma/common/stream_flags_common.c
    libs/lzma/src/liblzma/common/stream_flags_decoder.c
    libs/lzma/src/liblzma/common/stream_flags_encoder.c
    libs/lzma/src/liblzma/common/string_conversion.c
    libs/lzma/src/liblzma/common/vli_decoder.c
    libs/lzma/src/liblzma/common/vli_encoder.c
    libs/lzma/src/liblzma/common/vli_size.c
    libs/lzma/src/liblzma/delta/delta_common.c
    libs/lzma/src/liblzma/delta/delta_decoder.c
    libs/lzma/src/liblzma/delta/delta_encoder.c
    libs/lzma/src/liblzma/lz/lz_decoder.c
    libs/lzma/src/liblzma/lz/lz_encoder.c
    libs/lzma/src/liblzma/lz/lz_encoder_mf.c
    libs/lzma/src/liblzma/lzma/fastpos_table.c
    libs/lzma/src/liblzma/lzma/lzma2_decoder.c
    libs/lzma/src/liblzma/lzma/lzma2_encoder.c
    libs/lzma/src/liblzma/lzma/lzma_decoder.c
    libs/lzma/src/liblzma/lzma/lzma_encoder.c
    libs/lzma/src/liblzma/lzma/lzma_encoder_optimum_fast.c
    libs/lzma/src/liblzma/lzma/lzma_encoder_optimum_normal.c
    libs/lzma/src/liblzma/lzma/lzma_encoder_presets.c
    libs/lzma/src/liblzma/rangecoder/price_table.c
    libs/lzma/src/liblzma/simple/arm.c
    libs/lzma/src/liblzma/simple/arm64.c
    libs/lzma/src/liblzma/simple/armthumb.c
    libs/lzma/src/liblzma/simple/ia64.c
    libs/lzma/src/liblzma/simple/powerpc.c
    libs/lzma/src/liblzma/simple/riscv.c
    libs/lzma/src/liblzma/simple/simple_coder.c
    libs/lzma/src/liblzma/simple/simple_decoder.c
    libs/lzma/src/liblzma/simple/simple_encoder.c
    libs/lzma/src/liblzma/simple/sparc.c
    libs/lzma/src/liblzma/simple/x86.c
    libs/lzma/src/common/tuklib_cpucores.c
    libs/lzma/src/common/tuklib_physmem.c
)

add_library(lzma_static STATIC ${LZMA_SOURCES})
target_include_directories(lzma_static PUBLIC 
    libs/lzma/src/liblzma/api
    libs/lzma/src/common
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
# ZLIB - cerca libreria di sistema
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
    ${ZLIB_INCLUDE_DIR}
    include
)

target_link_libraries(tarc
    xxhash_static
    lzma_static
    ${ZLIB_LIBRARY}
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
message(STATUS "ZLIB: ${ZLIB_LIBRARY}")
message(STATUS "========================================")
message(STATUS "")
