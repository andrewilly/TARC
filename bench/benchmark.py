#!/usr/bin/env python3
"""
TARC Benchmark Suite
Tests compression performance across codecs, levels, and data types.
"""

import os
import sys
import subprocess
import tempfile
import shutil
import time
import json
import argparse
from pathlib import Path
from datetime import timedelta

TARC = os.environ.get("TARC_BIN", "./tarc")

DATA_SIZES = {
    "small":  1 * 1024 * 1024,
    "medium": 10 * 1024 * 1024,
    "large":  50 * 1024 * 1024,
}

LEVELS = [1, 7, 13, 19]

CODECS = {
    "auto":   [],
    "zstd":   ["--zstd"],
    "lzma":   ["--lzma"],
    "lz4":    ["--lz4"],
    "brotli": ["--brotli"],
    "store":  ["--store"],
}

RESULT_COLS = [
    "data_type", "size", "codec", "level",
    "compress_time_s", "compress_speed_mbps",
    "ratio_pct", "decompress_time_s", "decompress_speed_mbps",
]


def human_size(n: int) -> str:
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024:
            return f"{n:.2f} {unit}"
        n /= 1024
    return f"{n:.2f} TB"


def generate_data(tmpdir: str, name: str, size: int) -> str:
    path = os.path.join(tmpdir, name)
    chunk = bytearray(65536)

    if name.startswith("text_"):
        for i in range(len(chunk)):
            chunk[i] = 0x20 + (i % 95)
    elif name.startswith("binary_"):
        for i in range(len(chunk)):
            chunk[i] = (i * 7 + 13) & 0xFF
    elif name.startswith("random_"):
        chunk = bytearray(os.urandom(65536))
    elif name.startswith("mixed_"):
        half = len(chunk) // 2
        for i in range(half):
            chunk[i] = 0x20 + (i % 95)
        chunk[half:] = bytearray(os.urandom(half))
    else:
        for i in range(len(chunk)):
            chunk[i] = 0x20 + (i % 95)

    with open(path, "wb") as f:
        written = 0
        while written < size:
            f.write(chunk[: min(len(chunk), size - written)])
            written += len(chunk)
    return path


def run_bench(data_type: str, size: int, codec_name: str, level: int,
              archive: str, data_file: str) -> dict:
    result = {
        "data_type": data_type,
        "size": size,
        "codec": codec_name,
        "level": level,
    }

    codec_flags = CODECS[codec_name]
    level_flag = f"-c{level}"

    # --- Compression ---
    args = [TARC, level_flag] + codec_flags + [archive, data_file]
    t0 = time.perf_counter()
    proc = subprocess.run(
        args,
        capture_output=True,
        text=True,
        timeout=600,
    )
    t1 = time.perf_counter()
    dt = t1 - t0

    if proc.returncode != 0:
        stderr = proc.stderr.strip() or proc.stdout.strip()
        result["error"] = f"compress failed: {stderr}"
        return result

    result["compress_time_s"] = round(dt, 3)
    if dt > 0:
        result["compress_speed_mbps"] = round(size / (1024 * 1024) / dt, 2)
    else:
        result["compress_speed_mbps"] = 0

    # --- Ratio ---
    try:
        arc_size = os.path.getsize(archive)
        if size > 0:
            result["ratio_pct"] = round(100.0 * (1.0 - arc_size / size), 2)
        else:
            result["ratio_pct"] = 0
    except OSError:
        result["ratio_pct"] = 0

    # --- Decompression (use -t to test without writing to disk) ---
    args_test = [TARC, "-t", "--no-verify", archive]
    t0 = time.perf_counter()
    proc = subprocess.run(
        args_test,
        capture_output=True,
        text=True,
        timeout=600,
    )
    t1 = time.perf_counter()
    dt = t1 - t0

    if proc.returncode != 0:
        result["decompress_time_s"] = -1
        result["decompress_speed_mbps"] = 0
        result["error_d"] = f"decompress failed: {proc.stderr.strip() or proc.stdout.strip()}"
    else:
        result["decompress_time_s"] = round(dt, 3)
        if dt > 0:
            result["decompress_speed_mbps"] = round(size / (1024 * 1024) / dt, 2)
        else:
            result["decompress_speed_mbps"] = 0

    return result


def print_table(results: list, markdown: bool = False):
    if not results:
        return

    headers = [
        "Data", "Size", "Codec", "Lvl",
        "Comp(s)", "Comp(MB/s)", "Ratio(%)", "Decomp(s)", "Decomp(MB/s)",
    ]
    rows = []
    for r in results:
        if "error" in r:
            row = [
                r["data_type"], human_size(r["size"]),
                r["codec"], str(r["level"]),
                "ERR", "ERR", "ERR", "ERR", "ERR",
            ]
        else:
            row = [
                r["data_type"], human_size(r["size"]),
                r["codec"], str(r["level"]),
                f"{r['compress_time_s']:.3f}",
                f"{r['compress_speed_mbps']:.1f}",
                f"{r['ratio_pct']:.1f}",
                f"{r['decompress_time_s']:.3f}",
                f"{r['decompress_speed_mbps']:.1f}",
            ]
        rows.append(row)

    col_widths = [max(len(str(r[i])) for r in [headers] + rows) for i in range(len(headers))]
    sep = " | " if markdown else "  "

    def fmt_row(row):
        parts = []
        for i, cell in enumerate(row):
            if i in (1, 5, 6, 8) or (i == 3):
                parts.append(str(cell).rjust(col_widths[i]))
            else:
                parts.append(str(cell).ljust(col_widths[i]))
        return sep.join(parts)

    header_line = fmt_row(headers)
    under_line = sep.join(("─" * w for w in col_widths)) if not markdown else \
                 sep.join(("-" * w for w in col_widths))

    print("\n" + header_line)
    print(under_line)
    for row in rows:
        print(fmt_row(row))
    print()


def print_summary(results: list):
    totals = len(results)
    errors = [r for r in results if "error" in r]
    print(f"\n{'='*60}")
    print(f"  Total runs: {totals}  |  Errors: {len(errors)}")
    if errors:
        for e in errors:
            print(f"    - [{e['data_type']}/{e['codec']}@{e['level']}] {e.get('error', e.get('error_d', 'unknown'))}")

    ok = [r for r in results if "error" not in r]
    if ok:
        avg_comp = sum(r["compress_speed_mbps"] for r in ok) / len(ok)
        avg_decomp = sum(r["decompress_speed_mbps"] for r in ok) / len(ok)
        avg_ratio = sum(r["ratio_pct"] for r in ok) / len(ok)
        best_ratio = max(ok, key=lambda r: r["ratio_pct"])
        best_comp = max(ok, key=lambda r: r["compress_speed_mbps"])
        print(f"  Avg compression:  {avg_comp:.1f} MB/s")
        print(f"  Avg decompression: {avg_decomp:.1f} MB/s")
        print(f"  Avg ratio:        {avg_ratio:.1f}%")
        print(f"  Best ratio:       {best_ratio['ratio_pct']:.1f}%  ({best_ratio['codec']}@{best_ratio['level']}, {best_ratio['data_type']})")
        print(f"  Fastest compress:  {best_comp['compress_speed_mbps']:.1f} MB/s  ({best_comp['codec']}@{best_comp['level']}, {best_comp['data_type']})")
    print(f"{'='*60}\n")


def run_benchmarks(sizes: list, levels: list, codecs: list,
                   data_types: list, quick: bool = False) -> list:
    results = []
    tmpdir = tempfile.mkdtemp(prefix="tarc_bench_")
    print(f"Working directory: {tmpdir}")
    print(f"TARC binary: {TARC}")
    print()

    try:
        for dtype in data_types:
            for size_label, size_bytes in sizes:
                if quick and size_bytes > DATA_SIZES["small"]:
                    continue
                print(f"--- Generating data: {dtype}_{size_label} ({human_size(size_bytes)}) ---")
                data_file = generate_data(tmpdir, f"{dtype}_{size_label}.dat", size_bytes)

                for codec_name in codecs:
                    for level in levels:
                        archive = os.path.join(tmpdir, f"bench_{dtype}_{size_label}_{codec_name}_l{level}.strk")
                        print(f"  [{dtype}] {codec_name} @ level {level} ... ", end="", flush=True)
                        r = run_bench(dtype, size_bytes, codec_name, level, archive, data_file)
                        results.append(r)
                        if "error" in r:
                            print(f"ERR: {r['error']}")
                        else:
                            print(f"ok  ({r['compress_time_s']:.2f}s  {r['ratio_pct']:.1f}%  {r['compress_speed_mbps']:.0f} MB/s)")
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)

    return results


def main():
    global TARC
    parser = argparse.ArgumentParser(description="TARC Benchmark Suite")
    parser.add_argument("--tarc", default=TARC, help="Path to tarc binary")
    parser.add_argument("--quick", action="store_true", help="Only small data (1MB)")
    parser.add_argument("--json", help="Save results as JSON")
    parser.add_argument("--csv", help="Save results as CSV")
    parser.add_argument("--markdown", action="store_true", help="Print table in markdown format")
    parser.add_argument("--levels", nargs="+", type=int, default=LEVELS,
                        help=f"Levels to test (default: {LEVELS})")
    parser.add_argument("--codecs", nargs="+", default=list(CODECS.keys()),
                        help=f"Codecs: {list(CODECS.keys())}")
    parser.add_argument("--data-types", nargs="+", default=["text", "binary", "random"],
                        help="Data types: text, binary, random, mixed")
    parser.add_argument("--sizes", nargs="+", default=["medium"],
                        help="Sizes: small, medium, large")

    args = parser.parse_args()
    TARC = args.tarc

    if not os.path.isfile(TARC) and not shutil.which(TARC):
        print(f"Error: tarc binary not found: {TARC}")
        sys.exit(1)

    size_map = {k: v for k, v in DATA_SIZES.items()}
    sizes = [(s, size_map[s]) for s in args.sizes if s in size_map]
    if not sizes:
        sizes = [("medium", DATA_SIZES["medium"])]

    for c in args.codecs:
        if c not in CODECS:
            print(f"Warning: unknown codec '{c}', skipping")
    codecs = [c for c in args.codecs if c in CODECS]

    for d in args.data_types:
        if d not in ("text", "binary", "random", "mixed"):
            print(f"Warning: unknown data type '{d}', skipping")
    data_types = [d for d in args.data_types if d in ("text", "binary", "random", "mixed")]

    print(f"{'='*60}")
    print(f"  TARC Benchmark Suite")
    print(f"  Binary: {TARC}")
    print(f"  Data:   {', '.join(f'{d}/{s[0]}' for d in data_types for s in sizes)}")
    print(f"  Codecs: {', '.join(codecs)}")
    print(f"  Levels: {', '.join(str(l) for l in args.levels)}")
    print(f"{'='*60}\n")

    results = run_benchmarks(
        sizes=sizes,
        levels=args.levels,
        codecs=codecs,
        data_types=data_types,
        quick=args.quick,
    )

    print("\n" + "=" * 60)
    print("  RESULTS")
    print("=" * 60)
    print_table(results, markdown=args.markdown)
    print_summary(results)

    if args.json:
        with open(args.json, "w") as f:
            json.dump(results, f, indent=2)
        print(f"Results saved: {args.json}")

    if args.csv:
        import csv
        with open(args.csv, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=RESULT_COLS + ["error", "error_d"],
                               extrasaction="ignore")
            w.writeheader()
            w.writerows(results)
        print(f"Results saved: {args.csv}")


if __name__ == "__main__":
    main()
