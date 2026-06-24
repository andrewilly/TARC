## TARC STRIKE v2.10 — Initial Release

**T**he **A**dvanced **R**eal-time **C**ompressor is a solid-block compression archiver with 5 codecs, per-file auto-select, deduplication, self-extracting archives, and full streaming support.

### Features

- **5 codecs**: LZMA2, ZSTD, Brotli, LZ4/HC, STORE — per-file auto-select or manual override
- **Solid blocks**: Up to 1 GB chunks for maximum ratio
- **Deduplication**: XXH64 content hash — identical files stored once
- **Self-extracting archives**: `--sfx` creates standalone executables
- **Streaming I/O**: Files larger than RAM compress without loading fully
- **SIMD optimizations**: AVX2, SSE4.2, NEON for buffer ops and checksums
- **Anti-OOM**: Auto-detects RAM, caps dictionary/window/buffer allocations
- **Integrity checks**: xxHash checksums on every chunk, verified on extract
- **Path traversal protection**: Blocks `../` and absolute paths in archives
- **Filename validation**: Rejects null bytes, control characters, reserved names
- **Portable**: macOS (Intel + Apple Silicon), Linux x86_64, Windows x64

### v2.10 Highlights

- 38 test cases, 191 assertions — all pass, zero warnings
- Full CI pipeline: build, test, ASan+UBSan on every push
- Fuzz target (libFuzzer + standalone driver) with OSS-Fuzz config
- Benchmark suite: 60 combinations across 5 codecs, 4 levels, 4 data types
- English documentation website with CLI reference, codec guide, dev guide
- Clean `Makefile` + `CMakeLists.txt` dual build system

### Downloads

| Platform | File |
|----------|------|
| Linux x86_64 | [`tarc-v2.10-linux-x86_64`](https://github.com/anomalyco/TARC/releases/download/v2.10/tarc-v2.10-linux-x86_64) |
| macOS Universal | [`tarc-v2.10-macos-universal`](https://github.com/anomalyco/TARC/releases/download/v2.10/tarc-v2.10-macos-universal) |
| Windows x64 | [`tarc-v2.10-windows-x64.exe`](https://github.com/anomalyco/TARC/releases/download/v2.10/tarc-v2.10-windows-x64.exe) |
| **Checksums** | [`checksums.txt`](https://github.com/anomalyco/TARC/releases/download/v2.10/checksums.txt) |

### Quick Start

```bash
# Create an archive
./tarc -c7 backup.strk docs/ images/

# Extract everything
./tarc -x backup.strk

# List contents
./tarc -l backup.strk

# Self-extracting archive
./tarc -c9 --sfx myapp.strk src/
```

### Compression Levels

```
-c1  to  -c9    Balance speed and ratio
-c10 to -c19    Extreme presets (large dictionary, btultra strategy)
-cfast          Alias for -c1
-cbest          Alias for -c19
```

Default level is **7**.

### Build from Source

```bash
# Dependencies: zstd, lz4, xz, brotli, xxhash

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

### Documentation

- [Usage Guide](https://github.com/anomalyco/TARC/blob/main/docs/usage.md)
- [Codec Guide](https://github.com/anomalyco/TARC/blob/main/docs/codecs.md)
- [Installation](https://github.com/anomalyco/TARC/blob/main/docs/installation.md)
- [Format Spec](https://github.com/anomalyco/TARC/blob/main/STRK_SPEC.md)
- [Development Guide](https://github.com/anomalyco/TARC/blob/main/docs/development.md)
- [Man Page](https://github.com/anomalyco/TARC/blob/main/tarc.1)
