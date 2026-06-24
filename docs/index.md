---
layout: default
title: Home
nav_order: 1
---

# TARC STRIKE

**T**he **A**dvanced **R**eal-time **C**ompressor — a solid-block compression archiver with native LZMA2, ZSTD, Brotli, LZ4/HC, and STORE codecs.

## Quick Start

```bash
# Create an archive
tarc -c7 backup.strk docs/ images/

# Extract all files
tarc -x backup.strk

# List archive contents
tarc -l backup.strk

# Verify integrity
tarc -t backup.strk
```

## Key Features

- **Multi-codec engine** — Per-file auto-selection or manual override (5 codecs)
- **Solid compression** — Up to 1 GiB blocks for superior ratios
- **Deduplication** — Identical files stored once via XXH64 content hashing
- **Self-extracting archives** — Create standalone executables with `--sfx`
- **Streaming I/O** — Process files larger than available RAM
- **Security** — Path traversal protection, xxHash integrity, anti-OOM safeguards
- **Performance** — SIMD-accelerated (AVX2, SSE4.2, NEON) with LTO builds

## Codec Comparison

| Codec | Compress Speed | Decompress Speed | Ratio |
|---|---|---|---|
| LZ4 | ⚡ Fastest | ⚡ Fastest | Good |
| ZSTD | ⚡ Fast | ⚡ Fast | Better |
| LZMA2 | Moderate | ⚡ Fast | Best |
| Brotli | Moderate | ⚡ Fast | Best (text) |
| STORE | ⚡ Native | ⚡ Native | None |

## Documentation

- [Installation Guide](installation.md)
- [Usage Reference](usage.md)
- [Codec Guide](codecs.md)
- [Development Guide](development.md)
- [Format Specification](https://github.com/andrewilly/TARC/blob/main/STRK_SPEC.md)

## Project Links

- [GitHub Repository](https://github.com/andrewilly/TARC)
- [Releases](https://github.com/andrewilly/TARC/releases)
- [Issue Tracker](https://github.com/andrewilly/TARC/issues)
