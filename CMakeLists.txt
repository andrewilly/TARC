cmake_minimum_required(VERSION 3.20)
project(TARC VERSION 2.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Opzioni di compilazione
if(MSVC)
    add_compile_options(/W4 /O2 /MT)
else()
    add_compile_options(-Wall -Wextra -O3 -march=native -flto)
endif()

# Librerie esterne
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBLZMA REQUIRED liblzma)
pkg_check_modules(XXHASH REQUIRED libxxhash)

# Include directories
include_directories(
    include
    ${LIBLZMA_INCLUDE_DIRS}
    ${XXHASH_INCLUDE_DIRS}
)

# Sorgenti
set(SOURCES
    src/main.cpp
    src/engine.cpp
    src/io.cpp
    src/ui.cpp
    src/license.cpp
)

# Eseguibile
add_executable(tarc ${SOURCES})

target_link_libraries(tarc
    ${LIBLZMA_LIBRARIES}
    ${XXHASH_LIBRARIES}
    Threads::Threads
)

# Installazione
install(TARGETS tarc DESTINATION bin)
