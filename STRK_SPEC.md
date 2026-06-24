# TARC .strk Archive Format Specification v2.11

## 1. Overview

The `.strk` format is the native archive format of TARC STRIKE. It supports multiple compression codecs (LZMA2, ZSTD, LZ4/HC, Brotli, STORE), solid-block compression, deduplication, and self-extracting archives (SFX).

### 1.1 File Extension

`.strk` — TARC archive (Strike Archive).

### 1.2 Byte Order

All multi-byte integer fields are **little-endian**.

### 1.3 Alignment

All structures are packed on 1-byte boundaries (`#pragma pack(push, 1)`).

---

## 2. File Layout

A `.strk` file has three sections:

```
┌─────────────────────────────┐
│         Header (20 B)       │  ← Fixed at offset 0
├─────────────────────────────┤
│    Chunk Data (variable)    │  ← Starts at offset sizeof(Header)
│  ┌───────────────────────┐  │
│  │  ChunkHeader (20 B)   │  │
│  │  Compressed Data      │  │
│  │  ...                  │  │
│  │  END_MARKER (20 B)    │  │  ← All-zero ChunkHeader
│  └───────────────────────┘  │
├─────────────────────────────┤
│   TOC (Table of Contents)   │  ← Offset stored in Header.toc_offset
│  ┌───────────────────────┐  │
│  │  Entry 0 (44 B) + name│  │
│  │  Entry 1 (44 B) + name│  │
│  │  ...                  │  │
│  └───────────────────────┘  │
└─────────────────────────────┘
```

For SFX archives, the stub executable is prepended and a `SfxTrailer` is appended:

```
┌─────────────────────────────────┐
│  SFX Stub Executable (variable) │
├─────────────────────────────────┤
│  Header (20 B)                  │
│  Chunk Data (variable)          │
│  TOC (variable)                 │
├─────────────────────────────────┤
│  SfxTrailer (24 B)              │  ← Last 24 bytes of file
└─────────────────────────────────┘
```

---

## 3. Header

`sizeof(Header) = 20 bytes`

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 4 | `magic` | Magic number — ASCII `"TRC2"` (0x32435254) |
| 4 | 4 | `version` | Format version — currently `200` (v2.00), minimum `100` (v1.00) |
| 8 | 8 | `toc_offset` | File offset (in bytes) to the start of the TOC |
| 16 | 4 | `file_count` | Number of `FileEntry` records in the TOC |

### Validation Rules

- `magic` must equal `"TRC2"`
- `version` must be between `100` and `200` inclusive
- `toc_offset` must be >= `sizeof(Header)` (20)
- `file_count` should match the actual number of entries (not strictly validated at read)

---

## 4. Chunk Data

Immediately after the header, chunk data is written sequentially. Each chunk consists of a `ChunkHeader` followed by the compressed (or stored) data.

### 4.1 ChunkHeader

`sizeof(ChunkHeader) = 20 bytes`

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 4 | `codec` | Codec identifier (see §6) |
| 4 | 4 | `raw_size` | Uncompressed size of this chunk in bytes |
| 8 | 4 | `comp_size` | Compressed size of this chunk in bytes |
| 12 | 8 | `checksum` | XXH64 checksum of the **compressed** data (0 = skip verification) |

### 4.2 END_MARKER

A `ChunkHeader` with all fields set to zero (`{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}`) signals the end of chunk data. The decompressor treats this as end-of-archive-data.

### 4.3 Layout Rules

- For each real (non-duplicate) file, one or more chunks are written
- A `raw_size` of `0` means the file is zero-length; no chunk data follows for that file
- `TARC_MAX_CHUNK_SIZE` = 2 GiB (`2^31`); files larger than this are split into multiple chunks
- The solid block system may combine multiple files into a single chunk (see §7)

### 4.4 Chunk Reading

The decompressor reads chunks sequentially:

1. Read `ChunkHeader` (20 bytes)
2. If all bytes are zero → END_MARKER (stop reading chunks)
3. If `comp_size > TARC_MAX_CHUNK_SIZE` or `raw_size > TARC_MAX_CHUNK_SIZE` → reject as corrupted
4. If `comp_size > 0` and `checksum != 0`: read `comp_size` bytes, verify XXH64 == `checksum`
5. Decompress `comp_size` bytes to obtain `raw_size` bytes of output
6. Verify decompressed size matches `ch.raw_size`

---

## 5. TOC (Table of Contents)

The TOC is written at the end of the file. The `Header.toc_offset` field points to its start position. The TOC contains exactly `file_count` entries.

### 5.1 Entry

`sizeof(Entry) = 44 bytes`

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 8 | `offset` | File offset to this file's chunk data (0 for duplicates) |
| 8 | 8 | `orig_size` | Original uncompressed file size |
| 16 | 8 | `comp_size` | Compressed size on disk (for solid: size of entire solid block) |
| 24 | 8 | `xxhash` | XXH64 checksum of the original file data (0 = skip verification) |
| 32 | 4 | `timestamp` | Unix timestamp of the original file (seconds since epoch) |
| 36 | 4 | `duplicate_of_idx` | Index of the original file entry (valid only if `is_duplicate == 1`) |
| 40 | 2 | `name_len` | Length of the filename in bytes (1–4096) |
| 42 | 1 | `codec` | Codec identifier (see §6) |
| 43 | 1 | `is_duplicate` | `0` = original file, `1` = duplicate (deduplicated) |

After each `Entry`, the filename is stored as `name_len` raw bytes (not null-terminated on disk).

### 5.2 Entry Validation

- `name_len` must be between 1 and `TARC_MAX_NAME_LEN` (4096)
- Filename must not contain null bytes (embedded `\0`)
- Filename must not contain control characters (0x00–0x1F) except tab (0x09)
- Filename must pass `is_safe_filename()` validation
- Path traversal is detected by `sanitize_extract_path()` during extraction

---

## 6. Codec Identifiers

| ID | Name | Description |
|----|------|-------------|
| 0 | `ZSTD` | Zstandard compression |
| 1 | `LZMA` | LZMA2 compression (default) |
| 2 | `STORE` | No compression (stored as-is) |
| 3 | `LZ4` | LZ4 / LZ4HC compression |
| 4 | `BR` | Brotli compression |

### 6.1 STORE Special Case

- Files < 2048 bytes are always stored as separate STORE chunks (not added to solid buffer)
- Files < 4096 bytes always compress to STORE regardless of selected codec
- Incompressible data may cause codec fallback to STORE

---

## 7. Solid Mode

By default, TARC uses solid-block compression. Multiple small files are accumulated into a buffer. When the buffer exceeds the configurable threshold (typically 8–128 MB based on available RAM), the accumulated data is compressed as a single chunk.

### 7.1 Solid Block Rules

- All files in a solid block share the same `Entry.offset` (pointing to the same chunk) and `Entry.codec`
- The codec for the block is determined by the first file in the block (via `CodecSelector` or CLI override)
- During extraction, the entire solid chunk is decompressed at once, then individual files are extracted from the decompressed buffer
- Deduplicated files are never part of a solid block (they reference the original entry)

### 7.2 Solid Block Diagram

```
On disk:
  [ChunkHeader (LZMA, raw_size=X, comp_size=Y)]
  [Compressed data: file_a.txt + file_b.txt + file_c.txt]

In TOC:
  file_a.txt → offset = A (same chunk)
  file_b.txt → offset = A (same chunk)
  file_c.txt → offset = A (same chunk)
```

---

## 8. SFX (Self-Extracting Archive)

SFX archives prepend a stub executable and append an `SfxTrailer` to the standard `.strk` format.

### 8.1 SfxTrailer

`sizeof(SfxTrailer) = 24 bytes`

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 8 | `magic` | Magic — ASCII `"TARC_SFX"` |
| 8 | 8 | `archive_offset` | Byte offset from file start to the embedded `.strk` archive |
| 16 | 8 | `archive_size` | Size in bytes of the embedded `.strk` archive |

### 8.2 SFX Layout

```
┌───────────────────────────────────────┐
│  SFX Stub (.exe)                      │
├───────────────────────────────────────┤
│  .strk Archive (Header + Data + TOC)  │
├───────────────────────────────────────┤
│  SfxTrailer (24 bytes)                │  ← archive_offset points here
└───────────────────────────────────────┘
```

The stub executable opens itself, reads the last 24 bytes, validates `"TARC_SFX"` magic, extracts the embedded archive to a temporary file, then processes it with the standard extraction engine.

### 8.3 Validation

- `magic` must equal `"TARC_SFX"` (8 bytes)
- `archive_offset` must be < file_size
- `archive_offset + archive_size` must be <= file_size - `SFX_TRAILER_SIZE`

---

## 9. Deduplication

Files with identical XXH64 checksums are detected during compression:

- The first occurrence is stored normally
- Subsequent duplicates get `is_duplicate = 1` and `duplicate_of_idx` pointing to the original
- No chunk data is written for duplicates
- On extraction, duplicates are recreated from the extracted original file

---

## 10. Security

### 10.1 Path Traversal Protection

`sanitize_extract_path()` enforces:

- Null bytes in paths → rejected (empty return)
- `..` directory components → rejected (path traversal)
- On Windows: drive letter prefixes (e.g., `C:`) → rejected
- Leading `/` characters → stripped (absolute → relative)

### 10.2 Filename Safety

`is_safe_filename()` enforces:

- Length must be 1–4096 bytes
- No null bytes
- No control characters (0x00–0x1F) except tab

### 10.3 Size Limits

| Limit | Value | Description |
|-------|-------|-------------|
| `TARC_MAX_CHUNK_SIZE` | 2 GiB | Per-chunk uncompressed size |
| `TARC_MAX_FILE_SIZE` | 8 GiB | Per-file uncompressed size |
| `TARC_MAX_NAME_LEN` | 4096 | Maximum filename length in bytes |

---

## 11. Format Version History

| Version | Notes |
|---------|-------|
| 100 | v1.00 initial format |
| 103 | v1.03 (legacy support) |
| 104 | v1.04 added `timestamp` field to Entry |
| 200 | v2.00 added Brotli codec, updated solid block system |

---

## Appendix A: File Walkthrough

Creating a single-file archive `example.strk` containing `hello.txt` (5 bytes):

```
Offset  Content                     Description
------  -------                     -----------
0x0000  54 52 43 32                 magic "TRC2"
0x0004  C8 00 00 00                 version = 200
0x0008  [TOC offset]                toc_offset (patched at end)
0x0010  01 00 00 00                 file_count = 1
0x0014  02 00 00 00                 ChunkHeader.codec = STORE
0x0018  05 00 00 00                 ChunkHeader.raw_size = 5
0x001C  05 00 00 00                 ChunkHeader.comp_size = 5
0x0020  [XXH64 checksum]            ChunkHeader.checksum
0x0028  48 65 6C 6C 6F             "Hello"
0x002D  00 00 00 00 ...             END_MARKER (20 zero bytes)
0x0041  [offset=0x0014]             Entry.offset
         [orig_size=5]
         [comp_size=5]
         [xxhash of "Hello"]
         [timestamp]
         [duplicate_of_idx=0]
         [name_len=9]
         [codec=STORE]
         [is_duplicate=0]
0x006D  68 65 6C 6C 6F 2E 74 78 74 "hello.txt"
```

The TOC offset in the header points to `0x0041`.
