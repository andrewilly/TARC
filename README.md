# TARC STRIKE v2.10_OpenAi

Advanced Solid Compression Archiver

## 🚀 Caratteristiche Principali

- **Solid Block Compression** - Chunk da 1GB per massimo ratio
- **Deduplicazione** - XXH64 per identificare file identici
- **Smart Codec Selection** - LZMA/ZSTD/LZ4/Brotli/STORE automatico
- **SFX Archive** - Autoestrattore integrato (tarc.exe e' sia archiviatore che stub)
- **Windows Native I/O** - API native per migliori performance
- **Multi-threaded Compression** - Compressione parallela chunk
- **Wildcard Support** - Gestione `*.ext`, `nome.*`, `cartella\*.ext` su Windows
- **SIMD Accelerated** - AVX2/SSE4.2/NEON per copia buffer e checksum
- **Path Traversal Protection** - Sicurezza Zip Slip integrata
- **Streaming Compression** - File grandi compressi senza caricare in RAM

## 📋 Comandi

```bash
# Crea archivio (level 1-9, default 3)
tarc -c[N] archivio file...
tarc -cbest archivio file...

# Estrai
tarc -x archivio
tarc -x archivio "*.txt"

# Elenca contenuto
tarc -l archivio

# Testa integrità
tarc -t archivio
```

## ⚙️ Opzioni

| Opzione | Descrizione |
|--------|-----------|
| `--sfx` | Crea archivio autoestraente (.exe) |
| `--flat` | Estrai senza percorsi |
| `--force` | Sovrascrivi file esistenti |

## 🏗️ Build

### CMake (consigliato)
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target tarc
```

### Makefile (macOS/Linux)
```bash
make              # Compila tarc (release)
make test         # Compila ed esegue i test
make sfx          # Compila anche lo stub SFX
make debug        # Compila con simboli di debug
```

### Windows (MSVC)
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -A x64
cmake --build build --config Release --target tarc
```

L'eseguibile sarà in `build/tarc` (o `build/Release/tarc.exe` su Windows).

## 📦 Dipendenze

- liblzma (compressione LZMA)
- zstd (supporto legacy)
- lz4 (compressione veloce)
- xxhash (checksum)

## 📋 Struttura Progetto

```
tarc/
├── include/           # Header files
│   ├── types.h        # Tipi, error codes, Result
│   ├── engine.h       # Motore compressione
│   ├── io.h          # I/O archivio
│   ├── ui.h          # Interfaccia utente
│   └── license.h     # Gestione licenza
├── src/             # Sorgenti
│   ├── main.cpp      # Entry point, CLI
│   ├── engine.cpp   # Compressione
│   ├── io.cpp       # I/O
│   ├── ui.cpp       # UI
│   ├── license.cpp  # Licenza
│   └── stub.cpp     # SFX
└── CMakeLists.txt   # Build system
```

## 🔧 Novità v2.10_OpenAi

### Per-File Progress Output
- **Compressione** - Ogni file processato mostra `[+] [CODEC] filename size ratio%`
- **Estrazione** - Ogni file estratto mostra `[×] filename size`
- **Deduplicazione** - File duplicati mostrano `→ DEDUP` invece del ratio
- **Test integrità** - File verificati mostrano `[OK] filename size`

### Thread Control
- **`--threads N`** - Limita il numero di thread di compressione parallela
- **Auto-detect** - Default: rileva automaticamente il numero di core CPU
- **Semaphore-based** - Workers limitati senza sovraccarico del sistema

### Anti-OOM Streaming
- **Streaming LZMA2** - Compressione a memoria costante (~128MB) per file grandi
- **Streaming ZSTD** - Window log scalabile con limite RAM
- **Streaming Brotli** - Quality e window size adattivi
- **Streaming STORE** - Copia diretta da disco per file non comprimibili

### SIMD Acceleration
- **AVX2** - Copia buffer vettorizzata 256-bit
- **SSE4.2/SSE2** - Fallback per CPU meno recenti
- **NEON** - Supporto Apple Silicon ARM
- **Runtime detection** - Auto-rileva e seleziona il percorso ottimale

### Memory Safety
- **MemoryManager** - Auto-detect RAM e limiti sicuri per dizionario/window/buffer
- **LZMA2 dictionary** - Fino a 1GB, cappato alla RAM disponibile
- **ZSTD window** - Fino a 1GB, scalabile per long-range matching
- **Solid buffer** - Adattivo, 8MB-128MB in base alla RAM

### Security
- **Path Traversal Protection** - Blocco Zip Slip (attacchi `../` negli archivi)
- **Filename Validation** - No null bytes, caratteri di controllo, o nomi unsafe
- **xxHash Integrity** - Verifica checksum su ogni chunk decompresso
- **SFX Validation** - Offset e dimensione verificati nel trailer

### Codec Enhancements
- **LZMA2** - Dizionario fino a 1GB vs 64MB della versione 1.x
- **ZSTD** - Window log 23-30, strategia btultra/btultra2 ai livelli alti
- **Brotli** - Quality 0-11, window 1MB-64MB
- **LZ4/LZ4HC** - HC per livelli >= 4
- **Smart CodecSelector** - Auto-scelta basata su estensione e dimensione
- **Fallback a STORE** - Se la compressione non riduce (o OOM)

### SFX Enhancements
- **Self-Extracting integrato** - tarc.exe e' sia archiviatore che stub SFX
- **Menu interattivo** - Extract/List/Test/Help con menu numerico
- **Modalità silenziosa** - `--extract`, `--list`, `--test` da riga di comando
- **Temporary file cleanup** - Pulizia automatica dopo operazione

### Cross-Platform
- **Windows MSVC** - Supporto sperimentale (ZSTD/LZ4 via FetchContent)
- **macOS Intel/Apple Silicon** - Binary universale con CMake
- **Linux** - Build con GCC/Clang
- **C++17** - Standard moderno su tutte le piattaforme
- **64-bit I/O** - `_fseeki64`/`fseeko` per file > 2GB