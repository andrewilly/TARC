---
layout: default
title: Codecs
nav_order: 4
---

# Codec Guide

TARC supports five compression codecs. Each has different trade-offs between speed, ratio, and memory usage.

## LZMA2 (Default)

The default codec, based on LZMA SDK (7-Zip). Best compression ratio for general data.

| Property | Value |
|---|---|
| Algorithm | LZMA2 (LZ77 + range coding + Markov chain) |
| Dictionary | 4 MB – 1 GB (level-dependent, capped to available RAM) |
| Strategy | Standard (levels 1–6), Extreme (levels 7–19) |
| Compress Memory | ~3× dictionary size |
| Decompress Memory | ~dictionary size |
| Best for | Text, source code, documents, executables |

**Tuning:** At level 19 with 1 GB dictionary, LZMA2 achieves maximum ratio but uses significant memory. On low-RAM systems, dictionary is capped automatically.

## ZSTD

Developed by Facebook/Meta. Excellent balance of speed and ratio, especially for mixed data.

| Property | Value |
|---|---|
| Algorithm | ZSTD (LZ77 + Huffman + FSE) |
| Window | 8 MB – 1 GB (level-dependent) |
| Strategy | Default (1–9), btultra (10–15), btultra2 (16–19) |
| Compress Memory | ~window size |
| Decompress Memory | ~8 KB |
| Best for | PDF, database files, mixed binary/text |

**Tuning:** ZSTD decompression is extremely fast regardless of level, making it ideal for archives that are compressed once and decompressed often.

## LZ4 / LZ4HC

Extremely fast compression and decompression. LZ4HC trades compress speed for better ratio.

| Property | Value |
|---|---|
| Algorithm | LZ4 (byte-aligned LZ77) / LZ4HC (HC variant) |
| Levels 1–3 | LZ4 default (very fast) |
| Levels 4+ | LZ4HC (slower compress, better ratio) |
| Compress Memory | ~8 KB (LZ4), ~256 KB (LZ4HC) |
| Decompress Memory | ~8 KB |
| Best for | Real-time compression, logs, small files |

**Note:** LZ4's compression ratio is generally lower than LZMA2 or ZSTD, but it's unmatched for speed. Decompression runs at >1 GB/s on modern hardware.

## Brotli

Developed by Google. Excellent text compression, used in web protocols (WOFF2, HTTP).

| Property | Value |
|---|---|
| Algorithm | Brotli (LZ77 + Huffman + context modeling) |
| Quality | 0–11 (maps to level) |
| Window | 1 MB – 64 MB |
| Compress Memory | ~window × 2 |
| Decompress Memory | ~8 KB |
| Best for | Text, HTML, JSON, markdown |

**Tuning:** Quality 1–7 provides fast compression with good ratio. Quality 8–11 is very slow but can achieve slightly better ratio than LZMA2 on text.

## STORE

No compression — data is stored as-is. Used for already-compressed files (.jpg, .mp4, .zip, etc.) and tiny files (< 4 KB).

## Auto-Select Logic

When no codec override is given, TARC selects based on file extension:

```
├── Already compressed (.zip, .7z, .rar, .gz, .strk)
│   └── STORE
├── PDF, EPUB, XPS
│   └── ZSTD
├── Text / Source code (.txt, .cpp, .py, .md, .json, .xml, .html, .csv, ...)
│   └── LZMA2
├── Databases (.db, .sqlite, .mdb)
│   └── ZSTD
├── Images / Media (.png, .jpg, .mp4, .mp3, ...)
│   └── STORE (already compressed)
├── Office documents (.docx, .xlsx, .pptx)
│   └── LZMA2 (solid block)
├── Small files (< 64 KB)
│   └── LZ4
└── Everything else
    └── LZMA2
```

## Performance Table

Benchmarked on 10 MB data (macOS x86_64, single-thread). Run `python3 bench/benchmark.py` on your hardware.

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
