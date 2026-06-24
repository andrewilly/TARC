---
layout: default
title: Installation
nav_order: 2
---

# Installation Guide

## Pre-built Binaries

Download the latest release from the [Releases page](https://github.com/andrewilly/TARC/releases):

| Platform | File |
|---|---|
| Linux x86_64 | `tarc-<version>-linux-x86_64` |
| macOS Universal (Intel + Apple Silicon) | `tarc-<version>-macos-universal` |
| Windows x64 | `tarc-<version>-windows-x64.exe` |

```bash
chmod +x tarc-*
./tarc --help
```

## Build from Source

### Prerequisites

| Library | macOS | Debian/Ubuntu | Fedora | Windows (MSYS2) |
|---|---|---|---|---|
| zstd | `brew install zstd` | `apt install libzstd-dev` | `dnf install libzstd-devel` | `pacman -S mingw-w64-x86_64-zstd` |
| lz4 | `brew install lz4` | `apt install liblz4-dev` | `dnf install lz4-devel` | `pacman -S mingw-w64-x86_64-lz4` |
| xz (LZMA) | `brew install xz` | `apt install liblzma-dev` | `dnf install xz-devel` | `pacman -S mingw-w64-x86_64-xz` |
| brotli | `brew install brotli` | `apt install libbrotli-dev` | `dnf install brotli-devel` | `pacman -S mingw-w64-x86_64-brotli` |

xxHash is bundled (`include/xxhash.h` + `src/xxhash.c`).

### Quick Build (Makefile)

```bash
git clone https://github.com/andrewilly/TARC.git
cd TARC
make
```

### Build with CMake

```bash
mkdir build && cd build
cmake ..
make
```

### Platform-specific Notes

#### macOS

The Makefile auto-detects Intel (x86_64) and Apple Silicon (arm64). No additional flags needed.

```bash
brew install zstd lz4 xz brotli
make
```

The binary is built with native architecture optimization (`-march=native`).

#### Linux

```bash
# Debian / Ubuntu
sudo apt install build-essential libzstd-dev liblz4-dev liblzma-dev libbrotli-dev
make

# Fedora
sudo dnf install gcc-c++ libzstd-devel lz4-devel xz-devel brotli-devel
make
```

#### Windows (MinGW / MSYS2)

```bash
pacman -S mingw-w64-x86_64-{zstd,lz4,xz,brotli} mingw-w64-x86_64-{cmake,make}
make
```

### Build Options

| Command | Description |
|---|---|
| `make` | Release build with LTO (`-O3 -flto`) |
| `make debug` | Debug build (`-g -O0`) |
| `make ASAN=1` | Build with AddressSanitizer + UBSan |
| `make test` | Build and run test suite |
| `make sfx` | Build SFX stub separately |
| `make fuzz` | Build fuzz target |
| `make clean` | Remove build artifacts |
| `make install` | Install to `/usr/local/bin` |
