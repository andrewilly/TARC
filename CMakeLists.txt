cmake_minimum_required(VERSION 3.15)
project(TARC VERSION 2.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
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

# Sorgenti principali
set(TARC_SOURCES
    src/main.cpp
    src/engine.cpp
    src/io.cpp
    src/ui.cpp
    src/license.cpp
)

add_executable(tarc ${TARC_SOURCES})
target_include_directories(tarc PRIVATE include)

# -----------------------------------------------------------------------------
# Windows: usa librerie statiche
# -----------------------------------------------------------------------------
if(WIN32)
    message(STATUS "Windows: Building with static libraries")
    
    target_include_directories(tarc PRIVATE 
        libs/lzma/src/liblzma/api
        libs/xxhash
    )
    
    # XXHASH
    set(XXHASH_SOURCES libs/xxhash/xxhash.c)
    add_library(xxhash_static STATIC ${XXHASH_SOURCES})
    target_include_directories(xxhash_static PUBLIC libs/xxhash)
    
    # LZMA - solo file essenziali
    set(LZMA_SOURCES
        libs/lzma/src/liblzma/check/check.c
        libs/lzma/src/liblzma/check/crc32_fast.c
        libs/lzma/src/liblzma/check/crc32_table.c
        libs/lzma/src/liblzma/check/crc64_fast.c
        libs/lzma/src/liblzma/check/crc64_table.c
        libs/lzma/src/liblzma/common/alone_decoder.c
        libs/lzma/src/liblzma/common/alone_encoder.c
        libs/lzma/src/liblzma/common/auto_decoder.c
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
        libs/lzma/src/liblzma/common/filter_common.c
        libs/lzma/src/liblzma/common/filter_decoder.c
        libs/lzma/src/liblzma/common/filter_encoder.c
        libs/lzma/src/liblzma/common/index.c
        libs/lzma/src/liblzma/common/index_decoder.c
        libs/lzma/src/liblzma/common/index_encoder.c
        libs/lzma/src/liblzma/common/stream_decoder.c
        libs/lzma/src/liblzma/common/stream_encoder.c
        libs/lzma/src/liblzma/common/stream_flags_common.c
        libs/lzma/src/liblzma/common/stream_flags_decoder.c
        libs/lzma/src/liblzma/common/stream_flags_encoder.c
        libs/lzma/src/liblzma/common/vli_decoder.c
        libs/lzma/src/liblzma/common/vli_encoder.c
        libs/lzma/src/liblzma/common/vli_size.c
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
        libs/lzma/src/liblzma/simple/simple_coder.c
        libs/lzma/src/liblzma/simple/simple_decoder.c
        libs/lzma/src/liblzma/simple/simple_encoder.c
        libs/lzma/src/liblzma/simple/x86.c
        libs/lzma/src/common/tuklib_cpucores.c
        libs/lzma/src/common/tuklib_physmem.c
    )
    
    add_library(lzma_static STATIC ${LZMA_SOURCES})
    target_include_directories(lzma_static PUBLIC 
        libs/lzma/src/liblzma/api
        libs/lzma/src/common
    )
    target_compile_definitions(lzma_static PRIVATE 
        LZMA_API_STATIC
        _CRT_SECURE_NO_WARNINGS
    )
    
    target_link_libraries(tarc lzma_static xxhash_static)
    
# -----------------------------------------------------------------------------
# macOS: usa librerie di sistema
# -----------------------------------------------------------------------------
elseif(APPLE)
    message(STATUS "macOS: Using system libraries")
    
    find_path(LZMA_INCLUDE_DIR lzma.h PATHS /opt/homebrew/include /usr/local/include)
    find_library(LZMA_LIBRARY lzma PATHS /opt/homebrew/lib /usr/local/lib)
    
    find_path(XXHASH_INCLUDE_DIR xxhash.h PATHS /opt/homebrew/include /usr/local/include)
    find_library(XXHASH_LIBRARY xxhash PATHS /opt/homebrew/lib /usr/local/lib)
    
    target_include_directories(tarc PRIVATE ${LZMA_INCLUDE_DIR} ${XXHASH_INCLUDE_DIR})
    target_link_libraries(tarc ${LZMA_LIBRARY} ${XXHASH_LIBRARY})
    
    find_package(Threads REQUIRED)
    target_link_libraries(tarc Threads::Threads)
    
# -----------------------------------------------------------------------------
# Linux: usa pkg-config
# -----------------------------------------------------------------------------
else()
    message(STATUS "Linux: Using system libraries")
    
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBLZMA REQUIRED liblzma)
    pkg_check_modules(XXHASH REQUIRED libxxhash)
    
    target_include_directories(tarc PRIVATE ${LIBLZMA_INCLUDE_DIRS} ${XXHASH_INCLUDE_DIRS})
    target_link_libraries(tarc ${LIBLZMA_LIBRARIES} ${XXHASH_LIBRARIES})
    
    find_package(Threads REQUIRED)
    target_link_libraries(tarc Threads::Threads)
endif()

install(TARGETS tarc RUNTIME DESTINATION bin)

message(STATUS "")
message(STATUS "========================================")
message(STATUS "TARC Strike v${PROJECT_VERSION}")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "========================================")
message(STATUS "")
