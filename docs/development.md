---
layout: default
title: Development
nav_order: 5
---

# Development

## Building

```bash
make              # Release build
make debug        # Debug build
make test         # Build and run tests
make ASAN=1 test  # Build with sanitizers and run tests
make sfx          # Build standalone SFX stub
make fuzz         # Build fuzz target
```

## Testing

TARC uses [doctest](https://github.com/doctest/doctest) — a header-only C++ testing framework.

```bash
make test
```

This compiles and runs 38 test cases covering:
- Round-trip compress/extract (all codecs)
- Empty archives and zero-length files
- Truncated and corrupt archives
- Unicode filenames (UTF-8)
- Path traversal security
- Deduplication (three-way)
- Small files, large files, incompressible data
- Level comparisons
- No-verify mode
- Pattern matching on extract

### Sanitizer Builds

```bash
make ASAN=1 test   # AddressSanitizer + UndefinedBehaviorSanitizer
```

The CI pipeline runs sanitizer builds on every push. Zero warnings policy.

## Fuzzing

A libFuzzer target exercises archive parsing:

```bash
# With libFuzzer (LLVM Clang):
make fuzz && ./fuzz_archive corpus/

# Without libFuzzer (standalone driver with ASan+UBSan):
make fuzz && ./fuzz_archive seed1.bin seed2.bin
```

The fuzz target:
1. Writes random bytes to a `.strk` file
2. Calls `Engine::extract()` (test mode, no disk writes)
3. Calls `Engine::list()`
4. Handles all error paths gracefully

### OSS-Fuzz

Configuration for OSS-Fuzz is in `fuzz/ossfuzz/`:
- `Dockerfile` — Ubuntu-based builder with dependencies
- `build.sh` — Compiles fuzz target with OSS-Fuzz's compiler flags
- `project.yaml` — Project metadata for integration

## Benchmarking

```bash
python3 bench/benchmark.py                          # Full suite
python3 bench/benchmark.py --quick                  # Quick (1 MB only)
python3 bench/benchmark.py --json results.json      # Save as JSON
python3 bench/benchmark.py --csv results.csv        # Save as CSV
python3 bench/benchmark.py --levels 1 7 13 19       # Custom levels
python3 bench/benchmark.py --codecs zstd lz4        # Specific codecs
```

## Code Style

- C++17
- No warnings allowed (`-Wall -Wextra -Wpedantic`)
- Comments in English
- RAII for resource management
- Error handling via `TarcResult` return types
- SIMD intrinsics via runtime dispatch

## Project Structure

```
├── src/                # Source files
│   ├── main.cpp        # Entry point, CLI parsing, command dispatch
│   ├── engine.cpp      # Core engine: compress, extract, codec implementations
│   ├── io.cpp          # Archive I/O: header, TOC, chunk read/write
│   ├── ui.cpp          # Terminal UI: progress bars, colors, formatting
│   ├── sfx_stub.cpp    # Self-extracting stub entry point
│   ├── license.cpp     # License management
│   └── xxhash.c        # XXH64 checksum library (bundled)
├── include/            # Headers
│   ├── types.h         # TarcResult, Codec enum, binary structures
│   ├── engine.h        # Engine API declarations
│   ├── io.h            # I/O API declarations
│   ├── ui.h            # UI API declarations
│   ├── simd_opt.h      # SIMD-optimized buffer operations
│   ├── license.h       # License interface
│   ├── xxhash.h        # xxHash header
│   └── doctest/        # Doctest testing framework
├── test/               # Test suite (38 tests, 191 assertions)
├── fuzz/               # Fuzz target and OSS-Fuzz config
├── bench/              # Benchmark suite (Python)
└── docs/               # Documentation website
```

## Architecture

```
┌──────────┐     ┌──────────┐     ┌──────────┐
│  main    │────▶│  Engine  │────▶│   I/O    │
│  (CLI)   │     │(compress,│     │(read/write│
│          │     │ extract, │     │ archive)  │
│          │     │ list)    │     │          │
└──────────┘     └────┬─────┘     └──────────┘
                      │
               ┌──────▼──────┐
               │  Codecs     │
               │ LZMA2 ZSTD  │
               │ LZ4 Brotli  │
               │ STORE       │
               └─────────────┘
```

## Security

TARC implements several security measures:

- **Path traversal protection**: `IO::sanitize_extract_path()` blocks paths with `../` or absolute paths in archives
- **Filename validation**: `IO::is_safe_filename()` rejects null bytes, control characters, and reserved names
- **xxHash integrity**: Every chunk has a checksum verified on extract
- **Bounds checking**: All reads validate against `TARC_MAX_CHUNK_SIZE` and `TARC_MAX_FILE_SIZE`
- **Anti-OOM**: MemoryManager caps allocations to available RAM
