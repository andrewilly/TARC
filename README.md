# TARC STRIKE v2.00_OpenAi

Advanced Solid Compression Archiver

## 🚀 Caratteristiche Principali

- **Solid Block Compression** - Chunk da 256MB per massimo ratio
- **Deduplicazione** - XXH64 per identificare file identici
- **Smart Codec Selection** - LZMA/ZSTD/STORE automatico
- **SFX Archive** - Autoestrattore per Windows
- **Windows Native I/O** - API native per migliori performance

## 📋 Comandi

```bash
# Crea archivio (level 1-9, default 3)
tarc -c[N] archivio file...
tarc -cbest archivio file...

# Aggiungi file
tarc -a[N] archivio file...

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
cmake --build build -j$(nproc)
```

L'eseguibile sarà in `build/tarc.exe`.

## 📦 Dipendenze

- liblzma (compressione)
- zstd (backup)
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
│   ├── io.cpp      # I/O
│   ├── ui.cpp      # UI
│   ├── license.cpp # Licenza
│   └── stub.cpp    # SFX
└── CMakeLists.txt   # Build system
```

## 🔧 Novità v2.00_OpenAi

### Miglioramenti Core
- **Error Handling** - Sistema error codes robusto con `std::expected`
- **Typed Results** - `TarcResult` con codici di errore specifici
- **Progress Interface** - Callback per progress tracking
- **Cancellation Support** - Possibilità di annullare operazioni

### UI/UX Migliorata
- **Modern UI** - Colori ANSI avanzati (256 colori)
- **Progress Bar** - Classe dedicata con animazione
- **Spinner** - Indicatore attività
- **Table Output** - Formattazione tabellare
- **Thread-safe Output** - Mutex per output concurrent

### License Manager
- **Trial Keys** - Chiavi di prova generate automaticamente
- **Secure Storage** - Path OS-specifici
- **Validation** - Checksum integrato

### Internals
- **RAII Patterns** - Gestione risorse automatica
- **constexpr** - Costanti compile-time
- **nullptr checks** - Null safety
- **C++17** - Standard moderno

## 📊 Algoritmo

| Codec | Quando Usato | Target |
|-------|-------------|--------|
| LZMA | Testo, codice, JSON | Massimo ratio |
| ZSTD | Binari, dati | Bilanciato |
| STORE | Già compressi | Pass-through |

## 📄 Changelog

### v2.00_OpenAi
- Error code system completo
- Modern UI con ProgressBar/Spinner
- Trial key generation
- Cancellable operations
- Thread-safe output

### v1.05
- Brotli support
- LZMA optimizations

### v1.04
- Smart hybrid engine
- Database optimization

## 📝 Licenza

Copyright (C) 2026 André Willy Rizzo

Questo software è fornito COSÌ COM'È, senza garanzia di alcun tipo.