---
layout: default
title: Installation
nav_order: 2
---

# Installation

## Pre-built Binaries

Download from the [Releases page](https://github.com/andrewilly/TARC/releases):

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
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target tarc
```

The binary will be at `build/tarc` (or `build/Release/tarc.exe` on Windows).

### Build Options

| Option | Description |
|---|---|
| `make debug` | Debug symbols (`-g -O0`) |
| `make ASAN=1 test` | Build with AddressSanitizer + UBSan and run tests |
| `make fuzz` | Build fuzz target for archive parsing |
| `make sfx` | Build standalone SFX stub |
| `cmake -DTARC_BUILD_TESTS=ON` | Build test suite |

### Verify Installation

```bash
./tarc --version
./tarc --help
```
