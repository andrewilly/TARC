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

# Extract
tarc -x backup.strk

# List contents
tarc -l backup.strk

# Test integrity
tarc -t backup.strk
```

## Why TARC?

- **Multiple codecs** — Per-file auto-selection or manual override (5 codecs)
- **Solid compression** — Up to 1 GB blocks for better ratios
- **Deduplication** — Identical files stored once via XXH64 hash
- **Self-extracting** — Create standalone `.exe` archives with `--sfx`
- **Streaming** — Files larger than RAM compress without loading fully
- **Safe** — Path traversal protection, xxHash integrity, anti-OOM
- **Fast** — SIMD-accelerated (AVX2, SSE4.2, NEON)

## Quick Comparison

| Codec | Compress Speed | Decompress Speed | Ratio |
|---|---|---|---|
| LZ4 | ⚡ Fastest | ⚡ Fastest | Good |
| ZSTD | ⚡ Fast | ⚡ Fast | Better |
| LZMA2 | 🐢 Slow | ⚡ Fast | Best |
| Brotli | 🐢 Slow (high levels) | ⚡ Fast | Best |
| STORE | ⚡ Native | ⚡ Native | None |

## Next Steps

- [Installation Guide](installation.md)
- [Usage Reference](usage.md)
- [Codec Details](codecs.md)
- [Development](development.md)
