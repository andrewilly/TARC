<p align="center">
  <img src="https://img.shields.io/badge/TARC_STRIKE-v2.11-2ea44f?style=for-the-badge" alt="TARC v2.11"/>
</p>

<h1 align="center">TARC STRIKE</h1>
<p align="center">
  <strong>T</strong>he <strong>A</strong>dvanced <strong>R</strong>eal-time <strong>C</strong>ompressor<br/>
  <em>Solid-block compression archiver with native LZMA2, ZSTD, Brotli, LZ4/HC, and STORE codecs</em>
</p>

<p align="center">
  <a href="https://github.com/andrewilly/TARC/actions/workflows/main.yml"><img src="https://github.com/andrewilly/TARC/actions/workflows/main.yml/badge.svg" alt="CI"/></a>
  <a href="LICENSE.txt"><img src="https://img.shields.io/badge/license-MIT-blue.svg" alt="License"/></a>
  <a href="https://isocpp.org/"><img src="https://img.shields.io/badge/C%2B%2B-17-blue.svg" alt="C++17"/></a>
  <a href="https://github.com/andrewilly/TARC/releases/tag/v2.11"><img src="https://img.shields.io/github/v/release/andrewilly/TARC?color=green&label=release" alt="Release"/></a>
</p>

---

## Overview

TARC is a high-performance, multi-codec archiving tool designed for reliability, security, and speed. It combines solid-block compression with per-file codec auto-selection to deliver optimal compression ratios across diverse data types.

### Key Capabilities

| Feature | Description |
|---|---|
| **5 codecs** | LZMA2, ZSTD, Brotli, LZ4/HC, STORE — auto-select or manual override |
| **Solid blocks** | Up to 1 GiB chunks for maximum ratio |
| **Deduplication** | XXH64 content-hash — identical files stored once |
| **Self-extracting** | `--sfx` creates standalone executables |
| **Streaming I/O** | Files larger than RAM compress without loading fully |
| **SIMD acceleration** | AVX2, SSE4.2, NEON for buffer ops and checksums |
| **Anti-OOM** | Auto-detects available RAM, caps dictionary/window/buffer |
| **Integrity verification** | xxHash checksums on every chunk, verified on extraction |
| **Security** | Path traversal protection, filename validation, bounds checking |
| **Portable** | macOS (Intel + Apple Silicon), Linux, Windows |

---

## Quick Start

```bash
# Create an archive (level 7, default)
tarc -c7 backup.strk docs/ images/

# Extract everything
tarc -x backup.strk

# Extract only source files
tarc -x backup.strk "*.cpp" "*.h"

# List contents
tarc -l backup.strk

# Test integrity
tarc -t backup.strk
```

---

## Installation

### Download Pre-built Binary

Get the latest release from the [Releases page](https://github.com/andrewilly/TARC/releases):

| Platform | File |
|---|---|
| Linux x86_64 | `tarc-v2.11-linux-x86_64` |
| macOS Universal (Intel + Apple Silicon) | `tarc-v2.11-macos-universal` |
| Windows x64 | `tarc-v2.11-windows-x64.exe` |

```bash
chmod +x tarc-*
./tarc --help
```

### Build from Source

**Dependencies:** `libzstd`, `liblz4`, `liblzma`, `libbrotli` (enc + dec + common). xxHash is bundled.

<table>
<tr><th>macOS</th><th>Linux (Debian/Ubuntu)</th><th>Windows (MinGW)</th></tr>
<tr><td>

```bash
brew install zstd lz4 xz brotli
make
```

</td><td>

```bash
sudo apt install libzstd-dev liblz4-dev \
  liblzma-dev libbrotli-dev
make
```

</td><td>

```bash
pacman -S mingw-w64-x86_64-{zstd,lz4,xz,brotli}
make
```

</td></tr>
</table>

See [docs/installation.md](docs/installation.md) for detailed instructions.

---

## Commands

### `-c` — Create Archive

```bash
tarc -c[N] <archive.strk> <path>... [options]
```

| Level | Range | Use Case |
|---|---|---|
| `-c1` to `-c9` | Speed/ratio balance | Daily backups, general use |
| `-c10` to `-c19` | Extreme presets | Long-term archival, maximum ratio |
| `-cfast` | Alias for `-c1` | Quick compression |
| `-cbest` | Alias for `-c19` | Maximum compression |

Default: `-c7`.

### `-x` — Extract Files

```bash
tarc -x <archive.strk> [filter...] [options]
```

Supports glob-style filters: `*.txt`, `dir/*`, `data/*.bin`.

### Other Commands

| Command | Description |
|---|---|
| `-l <archive>` | List archive contents with codec and ratio |
| `-t <archive>` | Test integrity (verify xxHash checksums, no disk writes) |
| `--version` | Display version information |
| `--help` | Show detailed help |

### Options

| Option | Applies To | Description |
|---|---|---|
| `--zstd` / `--lzma` / `--lz4` / `--brotli` / `--store` | create | Force codec override |
| `--sfx` | create | Build self-extracting archive |
| `--threads N` | create | Parallel threads (default: auto) |
| `--output-dir <path>` | extract | Extract to directory |
| `--flat` | extract | Flatten directory structure |
| `--force` | extract | Overwrite existing files |
| `--no-verify` | create, extract, test | Skip xxHash verification |

---

## Codec Reference

TARC automatically selects the optimal codec per file type. Override with `--<codec>`.

| Codec | Best For | Speed | Ratio |
|---|---|---|---|
| **LZMA2** (default) | Text, source code, documents, executables | Slow compress, fast decompress | Best |
| **ZSTD** | PDF, databases, mixed binary/text | Fast compress, very fast decompress | Better |
| **Brotli** | HTML, JSON, markdown, web assets | Moderate compress, fast decompress | Best (text) |
| **LZ4/LZ4HC** | Logs, real-time, small files | Extremely fast | Good |
| **STORE** | Already-compressed data (jpg, mp4, zip) | Native speed | None |

### Auto-Select Logic

```
Already-compressed (.zip, .7z, .gz, .strk)  →  STORE
Documents (.pdf, .epub, .xps)               →  ZSTD
Text / source code (.txt, .cpp, .py, .md)   →  LZMA2
Databases (.db, .sqlite, .mdb, .accdb)      →  ZSTD
Media (.png, .jpg, .mp4, .mp3)              →  STORE
Office (.docx, .xlsx, .pptx)                →  LZMA2
Small files (< 64 KiB)                       →  LZ4
Everything else                              →  LZMA2
```

---

## Performance

Benchmarked on 10 MiB data (macOS x86_64, single thread). Run the full suite with `python3 bench/benchmark.py`.

| Codec | Level | Compress (MB/s) | Decompress (MB/s) | Ratio (text) |
|---|---|---|---|---|
| **LZ4** | 1 | 33 | 26 | 99.5% |
| **LZ4** | 7 | 21 | 44 | 99.6% |
| **ZSTD** | 1 | 30 | 39 | 100.0% |
| **ZSTD** | 7 | 29 | 20 | 100.0% |
| **ZSTD** | 19 | 16 | 24 | 100.0% |
| **Brotli** | 1 | 23 | 26 | 100.0% |
| **Brotli** | 7 | 26 | 31 | 100.0% |
| **LZMA2** | 1 | 14 | 25 | 100.0% |
| **LZMA2** | 7 | 4 | 33 | 100.0% |
| **LZMA2** | 19 | 5 | 24 | 100.0% |
| **STORE** | — | 30 | 37 | 0.0% |

---

## Project Structure

```
tarc/
├── src/              # Source code
│   ├── main.cpp      # Entry point, CLI parser
│   ├── engine.cpp    # Core: compression, decompression, codecs
│   ├── io.cpp        # Archive I/O: header, TOC, chunk management
│   ├── ui.cpp        # Terminal UI: progress bars, colors
│   └── sfx_stub.cpp  # SFX self-extracting stub
├── include/          # Headers
├── test/             # Doctest-based test suite (38 tests, 191 assertions)
├── fuzz/             # Fuzz target + OSS-Fuzz configuration
├── bench/            # Benchmark suite (Python)
└── docs/             # Documentation (GitHub Pages)
```

---

## Testing & Quality

```bash
make test              # Run all 38 test cases
make ASAN=1 test       # AddressSanitizer + UndefinedBehaviorSanitizer
make fuzz              # Build fuzz target
```

TARC maintains a **zero-warnings policy** with `-Wall -Wextra -Wpedantic`. The CI pipeline runs full builds, sanitizer tests, and fuzz targets on every push across three platforms.

---

## Changelog

- **v2.11** (2026-06-24) — Bug fixes, LTO optimization, CodecSelector improvements [(details)](RELEASE_v2.11.md)
- **v2.10** (2026-05-12) — Initial release [(details)](RELEASE_v2.10.md)

---

## License

Distributed under the **MIT License**. See [LICENSE.txt](LICENSE.txt) for details.

Copyright &copy; 2026 Andre Willy Rizzo
