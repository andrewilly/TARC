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
    add_compile_options(/DXXH_INLINE_ALL)
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
# Windows: usa vcpkg per librerie
# -----------------------------------------------------------------------------
if(WIN32)
    message(STATUS "Windows: Building with vcpkg libraries")
    
    find_package(liblzma REQUIRED)
    find_package(xxhash REQUIRED)
    
    target_link_libraries(tarc liblzma::liblzma xxhash::xxhash)

# -----------------------------------------------------------------------------
# macOS: usa librerie di sistema
# -----------------------------------------------------------------------------
elseif(APPLE)
    message(STATUS "macOS: Building for ${CMAKE_OSX_ARCHITECTURES}")
    
    if(CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64")
        set(HOMEBREW_PREFIX "/usr/local")
    else()
        set(HOMEBREW_PREFIX "/opt/homebrew")
    endif()
    
    find_path(LZMA_INCLUDE_DIR lzma.h PATHS ${HOMEBREW_PREFIX}/include /usr/include)
    find_library(LZMA_LIBRARY lzma PATHS ${HOMEBREW_PREFIX}/lib /usr/lib)
    
    find_path(XXHASH_INCLUDE_DIR xxhash.h PATHS ${HOMEBREW_PREFIX}/include /usr/include)
    find_library(XXHASH_LIBRARY xxhash PATHS ${HOMEBREW_PREFIX}/lib /usr/lib)
    
    if(NOT LZMA_INCLUDE_DIR OR NOT LZMA_LIBRARY)
        message(FATAL_ERROR "LZMA not found. Run: brew install xz")
    endif()
    
    if(NOT XXHASH_INCLUDE_DIR OR NOT XXHASH_LIBRARY)
        message(FATAL_ERROR "XXHASH not found. Run: brew install xxhash")
    endif()
    
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
if(WIN32)
    message(STATUS "Libraries: vcpkg (liblzma + xxhash)")
elseif(APPLE)
    message(STATUS "Libraries: Homebrew for ${CMAKE_OSX_ARCHITECTURES}")
else()
    message(STATUS "Libraries: System (pkg-config)")
endif()
message(STATUS "========================================")
message(STATUS "")
