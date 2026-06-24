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

Compression levels 1–19 (default: **7**). Levels 1–9 balance speed and ratio; levels 10–19 use extreme presets for maximum compression.

**Examples:**

```bash
# Default compression (level 7)
tarc -c7 backup.strk docs/ images/

# Fast compression with LZ4
tarc -c1 --lz4 quick.strk *.txt

# Maximum compression with self-extracting archive
tarc -cbest --sfx portable.exe src/

# Parallel compression (4 threads)
tarc -c13 --threads 4 big.strk videos/
```

### `-x` — Extract Files

```bash
tarc -x <archive.strk> [filter...] [options]
```

**Filters** support glob-style patterns:

```bash
tarc -x backup.strk                          # Extract everything
tarc -x backup.strk "*.cpp" "*.h"            # Sources only
tarc -x backup.strk "dir/*"                  # Files from a directory
tarc -x backup.strk "data/*.bin"             # Pattern matching
```

**Options:**

| Option | Description |
|---|---|
| `--output-dir <path>` | Extract to a specific directory |
| `--flat` | Flatten directory structure (no subdirectories) |
| `--force` | Overwrite existing files without prompting |
| `--no-verify` | Skip xxHash integrity check |

### `-l` — List Contents

```bash
tarc -l <archive.strk>
```

Displays each file with its codec, original size, compressed size, and compression ratio. Duplicate files are marked with `(DUPLICATE)`.

### `-t` — Test Integrity

```bash
tarc -t <archive.strk> [--no-verify]
```

Reads and decompresses all data, verifying xxHash checksums without writing any files to disk.

---

## Options Reference

| Option | Applies To | Description |
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

---

## Compression Levels

| Range | Category | Dictionary | Strategy | Use Case |
|---|---|---|---|---|
| 1–3 | Fast | 4–16 MiB | Standard | Quick backups, log archiving |
| 4–6 | Balanced | 32–64 MiB | Standard | Daily backups |
| 7–9 | High | 128 MiB | Extreme | Archival storage |
| 10–12 | Very High | 256 MiB | btultra | Long-term storage |
| 13–15 | Extreme | 512 MiB | btultra | Maximum ratio (slow) |
| 16–19 | Insane | 1 GiB | btultra2 | Ultimate ratio (very slow) |

---

## Codec Auto-Select

When no codec override is specified, TARC selects the optimal codec based on file extension:

| Extension | Codec | Rationale |
|---|---|---|
| `.txt`, `.cpp`, `.py`, `.md`, `.json`, `.xml`, `.csv` | LZMA2 | Best compression for text |
| `.pdf`, `.epub`, `.xps` | ZSTD | Efficient with already-compressed streams |
| `.png`, `.jpg`, `.mp4`, `.mp3`, `.zip` | STORE | Already compressed |
| `.docx`, `.xlsx`, `.pptx` | LZMA2 | ZIP-based office documents in solid blocks |
| `< 64 KiB files` | LZ4 | Small files benefit from speed |
| Everything else | LZMA2 | Best all-around ratio |

Override with `--zstd`, `--lzma`, `--lz4`, `--brotli`, or `--store`.

---

## SFX Mode

Self-extracting archives bundle the TARC stub with the compressed data into a single executable:

```bash
tarc -c9 --sfx myapp.exe src/ docs/
```

The resulting executable supports the same commands as TARC:

```bash
./myapp.exe                    # Interactive menu
./myapp.exe --extract         # Extract all files
./myapp.exe --list             # List contents
./myapp.exe --test             # Test integrity
```

SFX archives use the current TARC binary as their stub. On extraction, they read the embedded archive from themselves and decompress directly.

---

## Exit Codes

| Code | Meaning |
|---|---|
| 0 | Success |
| 1 | Error — see error message for details |
