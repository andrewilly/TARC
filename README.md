# TARC STRIKE v2.00_OpenAi

Advanced Solid Compression Archiver

## 🚀 Caratteristiche Principali

- **Solid Block Compression** - Chunk da 1GB per massimo ratio
- **Deduplicazione** - XXH64 per identificare file identici
- **Smart Codec Selection** - LZMA/ZSTD/STORE automatico
- **SFX Archive** - Autoestrattore per Windows
- **Windows Native I/O** - API native per migliori performance
- **Multi-threaded Compression** - Compressione parallela chunk
- **Wildcard Support** - Gestione `*.ext`, `nome.*`, `cartella\*.ext` su Windows

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

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target tarc
```

L'eseguibile sarà in `build/tarc.exe`.

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

## 🔧 Novità v2.00_OpenAi

### Fix Critici (questa sessione)
- **Buffer decompressione** - Rimosso limite fisso 256MB, ora dinamico
- **Divisione per zero** - Protetta in ProgressBar
- **ProgressBar statica** - Sostituita con `unique_ptr` per aggiornamento totale
- **Gestione file grandi** - Controllo overflow `SIZE_MAX`
- **Codice morto** - Rimosso `CodecSelector::init()` inutile
- **Pulizia comandi** - Rimossi `-a` (aggiunta) e `-d` (eliminazione) non compatibili con archiviazione solida

### UI/UX Migliorata
- **Progress bar** - Allargata a 40 caratteri con velocità MB/s e ETA
- **Pulizia riga** - Escape `\x1b[2K` per evitare caratteri residui
- **Formattazione dimensioni** - B, KB, MB, GB, TB
- **Calcolo rapporto compressione** - Percentuale

### Internals
- **RAII Patterns** - Gestione risorse automatica
- **Thread-safe Output** - Mutex per output concorrente
- **Error Handling** - Sistema error codes robusto
- **C++17** - Standard moderno