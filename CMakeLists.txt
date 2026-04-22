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

# Cerca librerie
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBLZMA REQUIRED liblzma)
pkg_check_modules(XXHASH REQUIRED libxxhash)
pkg_check_modules(ZLIB REQUIRED zlib)  # Aggiunto per CRC32

# Sorgenti
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
    ${LIBLZMA_INCLUDE_DIRS}
    ${XXHASH_INCLUDE_DIRS}
    ${ZLIB_INCLUDE_DIRS}
    include
)

target_link_libraries(tarc
    ${LIBLZMA_LIBRARIES}
    ${XXHASH_LIBRARIES}
    ${ZLIB_LIBRARIES}
)

find_package(Threads REQUIRED)
target_link_libraries(tarc Threads::Threads)

install(TARGETS tarc RUNTIME DESTINATION bin)

message(STATUS "")
message(STATUS "========================================")
message(STATUS "TARC Strike v2.0")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "Libraries: LZMA + XXHASH + ZLIB")
message(STATUS "========================================")
message(STATUS "")
