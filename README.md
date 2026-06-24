# TARC STRIKE

**T**he **A**dvanced **R**eal-time **C**ompressor — Solid-block compression archiver with native LZMA2, ZSTD, Brotli, LZ4/HC, and STORE codecs.

[![CI](https://github.com/andrewilly/TARC/actions/workflows/main.yml/badge.svg)](https://github.com/andrewilly/TARC/actions/workflows/main.yml)
[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://isocpp.org/)

---

## Features

| Capability | Description |
|---|---|
| **5 codecs** | LZMA2, ZSTD, Brotli, LZ4/HC, STORE — per-file auto-select or override |
| **Solid blocks** | Up to 1 GB chunks for maximum ratio |
| **Deduplication** | XXH64 content hash — identical files stored once |
| **Self-extracting** | `--sfx` creates standalone .exe archives |
| **Streaming** | Files larger than RAM compress without loading fully |
| **SIMD** | AVX2, SSE4.2, NEON for buffer ops and checksums |
| **Anti-OOM** | Auto-detects RAM, caps dictionary/window/buffer |
| **Integrity** | xxHash checksums on every chunk, verified on extract |
| **Security** | Path traversal protection, filename validation |
| **Portable** | macOS (Intel + Apple Silicon), Linux, Windows |

## Quick Start

```bash
# Create an archive
tarc -c7 backup.strk docs/ images/

# Extract everything
tarc -x backup.strk

# Extract only .txt files
tarc -x backup.strk "*.txt"

# List contents
tarc -l backup.strk

# Test integrity
tarc -t backup.strk
```

## Install

### macOS (Homebrew)
```bash
# Coming soon: brew install tarc
```

### Linux (deb/rpm)
```bash
# Coming soon: apt install tarc / dnf install tarc
```

### Download binary
Get pre-built binaries from the [Releases page](https://github.com/anomalyco/TARC/releases):

| Platform | File |
|---|---|
| Linux x86_64 | `tarc-<version>-linux-x86_64` |
| macOS Universal | `tarc-<version>-macos-universal` |
| Windows x64 | `tarc-<version>-windows-x64.exe` |

### Build from source

**Requires:** libzstd, liblz4, liblzma, libbrotli (enc+dec+common), xxhash (bundled)

```bash
# macOS
brew install zstd lz4 xz brotli
make

# Linux (Debian/Ubuntu)
sudo apt install libzstd-dev liblz4-dev liblzma-dev libbrotli-dev
make

# Windows (MinGW)
pacman -S mingw-w64-x86_64-{zstd,lz4,xz,brotli}
make
```

See [docs/installation.md](docs/installation.md) for detailed instructions.

## Compression Levels

```
-c1  to  -c9    Balance speed and ratio
-c10 to -c19    Extreme presets (large dictionary, btultra strategy)
-cfast          Alias for -c1
-cbest          Alias for -c19
```

Default level is **7**.

## Codec Override

By default TARC selects the best codec per file type. Override with:

```
--zstd          Good for mixed/binary data
--lzma          Best ratio (default)
--lz4           Fastest compression/decompression
--brotli        Good for text
--store         No compression
```

## Performance

Benchmarked on 10 MB data (macOS x86_64, single thread):

| Codec | Level | Compress (MB/s) | Decompress (MB/s) | Ratio (text) |
|---|---|---|---|---|
| LZ4 | 1 | 33 | 26 | 99.5% |
| LZ4 | 7 | 21 | 44 | 99.6% |
| ZSTD | 1 | 30 | 39 | 100.0% |
| ZSTD | 7 | 29 | 20 | 100.0% |
| ZSTD | 19 | 16 | 24 | 100.0% |
| Brotli | 1 | 23 | 26 | 100.0% |
| Brotli | 7 | 26 | 31 | 100.0% |
| LZMA2 | 1 | 14 | 25 | 100.0% |
| LZMA2 | 7 | 4 | 33 | 100.0% |
| LZMA2 | 19 | 5 | 24 | 100.0% |
| STORE | — | 30 | 37 | 0.0% |

Run the full suite: `python3 bench/benchmark.py`

## Documentation

| Resource | Description |
|---|---|
| [Usage Guide](docs/usage.md) | Full CLI reference with examples |
| [Codec Guide](docs/codecs.md) | Codec details, tuning, trade-offs |
| [Installation](docs/installation.md) | Build from source on all platforms |
| [Format Spec](STRK_SPEC.md) | .strk archive binary format |
| [Man Page](tarc.1) | `man tarc` |
| [Development](docs/development.md) | Build, test, fuzz, contribute |

## Project Structure

```
├── src/            # Source code
│   ├── main.cpp    # Entry point, CLI parsing
│   ├── engine.cpp  # Compression, decompression, codecs
│   ├── io.cpp      # Archive I/O, TOC read/write
│   ├── ui.cpp      # Terminal UI, progress bar
│   └── sfx_stub.cpp # SFX self-extracting stub
├── include/        # Headers
├── test/           # Doctest-based test suite
├── fuzz/           # Fuzz target + OSS-Fuzz config
├── bench/          # Benchmark suite
└── docs/           # Documentation website
```

## Testing

```bash
make test          # Run all 38 test cases
make ASAN=1 test   # Compile and run with AddressSanitizer + UBSan
make fuzz          # Build fuzz target (or: make fuzz && ./fuzz_archive seed/)
```

## Fuzzing

A libFuzzer target exercises archive parsing with random data:

```bash
# With libFuzzer (LLVM Clang)
make fuzz && ./fuzz_archive corpus/

# Standalone mode (Apple Clang)
make fuzz && ./fuzz_archive seed1.bin seed2.bin
```

OSS-Fuzz configuration is in `fuzz/ossfuzz/`.

## License

MIT — see [LICENSE](LICENSE).
