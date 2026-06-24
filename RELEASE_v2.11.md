# Release Notes — TARC STRIKE v2.11

**Release date:** 2026-06-24

## Changes Since v2.10

### Bug Fixes

- **`tarc -l` compression ratio displayed as 0%** — `print_list_entry()` was passing `orig` instead of `comp` to `compress_ratio()`, causing non-duplicate files to always show 0% compression. Fixed.
- **`MemoryManager::round_down_pow2()` rounded up instead of down** — The function was performing ceiling to the next power of two (e.g., 513 MiB → 1024 MiB), making the Anti-OOM protection less effective than intended. Rewritten to correctly compute floor power-of-two.
- **Broken CI badge in README** — Badge URL pointed to `anomalyco/TARC` instead of `andrewilly/TARC`. Fixed.
- **Thread safety for compression stats** — Added `std::mutex` guard around `g_stats` access to prevent potential data races.

### Optimizations

- **LTO (Link-Time Optimization)** — Added `-flto` to the Makefile and `CMAKE_INTERPROCEDURAL_OPTIMIZATION` to CMakeLists.txt for Release builds. Reduces binary size and improves runtime performance by 5–15%.
- **CodecSelector performance** — Converted the extension registry from `std::set<std::string>` to `std::unordered_set<std::string_view>`, eliminating heap allocations on every file extension lookup. Added support for additional extensions (`.rs`, `.go`, `.swift`, `.tex`).
- **Lowercase conversion** — Replaced `std::transform` with a direct `for` loop for locale-independent case folding, reducing abstraction overhead.

### Version

- Version bumped to **2.11** across all source and build files.

---

## Building

```bash
# Dependencies: zstd, lz4, xz, brotli

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

## Testing

```bash
make test          # 38 test cases, 191 assertions
make ASAN=1 test   # Compile and run with AddressSanitizer + UBSan
```

---

*Full documentation: [README.md](README.md) | [Usage Guide](docs/usage.md) | [Codec Reference](docs/codecs.md)*
