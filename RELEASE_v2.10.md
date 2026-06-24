## TARC STRIKE v2.10 — Initial Release

**Release date:** 2026-05-12

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

### Downloads

See the [Releases page](https://github.com/andrewilly/TARC/releases/tag/v2.10).

### Build from Source

```bash
# Dependencies: zstd, lz4, xz, brotli, xxhash (bundled)

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
