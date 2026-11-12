# Release Notes — TARC STRIKE v2.12.1

**Release date:** 2026-11-12

## Changes Since v2.12

Patch release. No functional changes: the archive format, the CLI and the codec
behaviour are identical to v2.12. Only the Windows packaging is fixed.

### Windows: self-contained executable

- **The v2.12 Windows executable did not start on a clean Windows Server 2019.**
  It imported the MSVC runtime dynamically — `MSVCP140.dll`, `VCRUNTIME140.dll`
  and `VCRUNTIME140_1.dll` — because CMake's default for MSVC is `/MD`. Those
  DLLs are not part of Windows; they are installed by the Visual C++
  Redistributable, which a freshly provisioned server does not have. The loader
  failed before any TARC code ran.
- The Windows build now links the C/C++ runtime **statically (`/MT`)**. The
  setting is applied before the `FetchContent` dependencies, so zstd, lz4, xz,
  brotli and xxhash are built with the same runtime (mixing `/MT` and `/MD`
  breaks linking). The result is a single executable whose only imports are
  standard Windows system DLLs.
- **`_WIN32_WINNT` and `WINVER` are pinned to `0x0A00`** (Windows 10 /
  Server 2016+) for all Windows targets. This prevents the binary from
  implicitly requiring APIs newer than the intended target when it is built with
  a recent Windows SDK — the CI runner is Windows Server 2025 with a current
  MSVC toolset.

### CI

- New Windows-only step: `dumpbin /dependents` on the produced `tarc.exe` fails
  the build if the executable imports the MSVC redistributable runtime
  (`MSVCP`, `VCRUNTIME`, `api-ms-win-crt`). The check also requires
  `KERNEL32.dll` to be present in the dependency list, so a `dumpbin` that
  produced no output cannot make the check pass vacuously.

### Unchanged

- **macOS and Linux**: no build or behaviour change. The binaries are rebuilt
  only because the version string changed.
- Archive format, deduplication, `--force`, `--threads`, `--verify`, SFX and all
  the v2.12 fixes are untouched.

### Version

- Version bumped to **2.12.1** across all source and build files.

---

## Building

```bash
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
make test          # 49 test cases, 250 assertions
make ASAN=1 test   # Compile and run with AddressSanitizer + UBSan
```

---

*Full documentation: [README.md](README.md) | [Usage Guide](docs/usage.md) | [Codec Reference](docs/codecs.md)*
