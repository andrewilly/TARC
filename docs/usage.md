---
layout: default
title: Usage
nav_order: 3
---

# Usage Reference

## Commands

### `-c` — Create Archive

```bash
tarc -c[N] <archive.strk> <file|dir>... [options]
```

Compression levels 1–19 (default: 7). Levels 1–9 balance speed and ratio; 10–19 use extreme presets.

**Examples:**
```bash
tarc -c7 backup.strk docs/ images/          # Default level
tarc -c1 --lz4 quick.strk *.txt              # Fast compression
tarc -cbest --sfx portable.exe src/          # Max compression + SFX
tarc -c13 --threads 4 big.strk videos/       # Parallel compression
```

### `-x` — Extract Files

```bash
tarc -x <archive.strk> [filter...] [options]
```

**Filters:**
```bash
tarc -x backup.strk                          # Extract everything
tarc -x backup.strk "*.cpp" "*.h"            # Sources only
tarc -x backup.strk "dir/*"                  # Specific directory
```

**Options:**
```bash
tarc -x backup.strk --output-dir ./restore   # Extract to directory
tarc -x backup.strk --flat                   # Flatten paths
tarc -x backup.strk --force                  # Overwrite existing
tarc -x backup.strk --no-verify              # Skip integrity check
```

### `-l` — List Contents

```bash
tarc -l <archive.strk>
```

Shows each file with codec, size, and compression ratio. Duplicates marked `(DUPLICATE)`.

### `-t` — Test Integrity

```bash
tarc -t <archive.strk> [--no-verify]
```

Reads and decompresses all data, verifying xxHash checksums without writing to disk.

## Options Reference

| Option | Applies to | Description |
|---|---|---|
| `--zstd` | create | Force ZSTD compression |
| `--lzma` | create | Force LZMA2 compression (default) |
| `--lz4` | create | Force LZ4/LZ4HC compression |
| `--brotli` | create | Force Brotli compression |
| `--store` | create | No compression (store only) |
| `--sfx` | create | Create self-extracting archive |
| `--threads N` | create | Parallel compression threads (default: auto) |
| `--no-verify` | create, extract, test | Skip xxHash verification |
| `--output-dir <path>` | extract | Extract to directory |
| `--flat` | extract | Flatten directory structure |
| `--force` | extract | Overwrite existing files |

## Compression Levels Detail

| Level | Range | Dictionary | Strategy | Use Case |
|---|---|---|---|---|
| 1–3 | Fast | 4–16 MB | Standard | Quick backups, logs |
| 4–6 | Balanced | 32–64 MB | Standard | Daily backups |
| 7–9 | High | 128 MB | Extreme | Archival |
| 10–12 | Very High | 256 MB | btultra | Long-term storage |
| 13–15 | Extreme | 512 MB | btultra | Maximum ratio (slow) |
| 16–19 | Insane | 1 GB | btultra2 | Ultimate ratio (very slow) |

## Codec Override

By default, TARC selects the best codec per file extension:

| Extension | Codec | Reason |
|---|---|---|
| `.txt`, `.cpp`, `.py`, `.md`, `.json`, `.xml`, `.csv` | LZMA2 | Best text compression |
| `.pdf`, `.epub` | ZSTD | Handles already-compressed streams |
| `.png`, `.jpg`, `.mp4`, `.mp3` | STORE | Already compressed |
| `.docx`, `.xlsx`, `.pptx` | LZMA2 | ZIP-based office docs |
| `< 64 KB files` | LZ4 | Small files → fast codec |
| Everything else | LZMA2 | Default best ratio |

Override with `--zstd`, `--lzma`, `--lz4`, `--brotli`, or `--store`.

## Exit Codes

| Code | Meaning |
|---|---|
| 0 | Success |
| 1 | Error (see error message) |
