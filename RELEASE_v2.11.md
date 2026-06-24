## TARC STRIKE v2.11 — Bug Fix & Optimization Release

### Changes since v2.10

#### Bug Fixes

- **UI: Compression ratio always 0% in `tarc -l`** — `print_list_entry()` passava `orig` invece di `comp` a `compress_ratio()`, mostrando sempre 0% di compressione per file non-duplicati. Corretto.
- **MemoryManager: `round_down_pow2()` arrotondava in su invece che in giù** — La funzione `round_down_pow2()` in realtà faceva ceiling alla potenza di 2 successiva, causando un Anti-OOM meno efficace (es. 513MB → 1024MB invece del floor 512MB). Riscritto l'algoritmo per eseguire floor power-of-2 correttamente.
- **Badge CI rotto nel README** — Il badge CI puntava a `github.com/anomalyco/TARC` invece di `andrewilly/TARC`.
- **Race condition su `g_stats`** — Aggiunto `std::mutex` a protezione delle statistiche di compressione per accesso thread-safe.

#### Optimizations

- **LTO (Link-Time Optimization)**: Aggiunto `-flto` al `Makefile` e `CMAKE_INTERPROCEDURAL_OPTIMIZATION` al `CMakeLists.txt` per i build Release. Riduce la dimensione del binario e migliora le performance del 5-15%.
- **CodecSelector ottimizzato**: Convertito il set di estensioni da `std::set<std::string>` a `std::unordered_set<std::string_view>`, eliminando allocazioni per ogni lookup di estensione file. Aggiunte nuove estensioni (.rs, .go, .swift, .tex).
- **Codice sorgente**: Sostituito `std::transform` su ogni char con un loop `for` diretto per la conversione lowercase — più leggibile e senza overhead di allocazione intermedia.

#### Version Bump

- Versione aggiornata a **2.11** in tutti i file (CMakeLists.txt, Makefile, main.cpp, ui.cpp).

### Building

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

### Testing

```bash
make test          # 38 test cases
make ASAN=1 test   # Compile and run with AddressSanitizer + UBSan
```
