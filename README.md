# TARC v1.02 — Hybrid Compression Archiver

Archiver ad alte prestazioni con **selezione automatica del codec** in base al tipo di file.

## Struttura del Progetto

```
tarc/
├── CMakeLists.txt          # Build system (FetchContent per tutte le dipendenze)
├── include/
│   ├── types.h             # Strutture dati condivise, enum Codec
│   ├── ui.h                # Interfaccia UI (colori, stampa, help)
│   ├── license.h           # License manager
│   ├── io.h                # I/O archivio (TOC, helpers filesystem)
│   └── engine.h            # Engine compressione + CodecSelector
└── src/
    ├── main.cpp            # Entry point, parsing argomenti
    ├── ui.cpp              # Implementazione UI
    ├── license.cpp         # Implementazione License
    ├── io.cpp              # Implementazione I/O
    └── engine.cpp          # Implementazione Engine + codec ibrido
```

## Algoritmo Ibrido

| Codec | Quando viene usato                          | Caratteristica        |
|-------|---------------------------------------------|-----------------------|
| LZ4   | File già compressi (`.zip`, `.jpg`, `.mp4`) | Velocità massima      |
| ZSTD  | File generici, binari, dati                 | Bilanciato (default)  |
| LZMA  | Testo, codice sorgente (`.cpp`, `.json`)    | Ratio massimo         |
| NONE  | File ad alta entropia (rilevati runtime)    | Nessun overhead       |

La selezione avviene in due fasi:
1. **Per estensione** — decisione rapida e deterministica
2. **Per entropia** — analisi dei primi 4KB per rilevare file già compressi a runtime

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

L'eseguibile finale si trova in `build/tarc`.

## Dipendenze (scaricate automaticamente via FetchContent)

- [zstd](https://github.com/facebook/zstd) v1.5.6
- [lz4](https://github.com/lz4/lz4) v1.10.0
- [xz/liblzma](https://github.com/tukaani-project/xz) v5.6.3
- [xxhash](https://github.com/Cyan4973/xxHash) v0.8.2

## Utilizzo

```
tarc -c[N]  archivio  file...    Crea archivio (livello 1-22, default 3)
tarc -a[N]  archivio  file...    Aggiungi file
tarc -x     archivio             Estrai tutto
tarc -l     archivio             Elenca contenuto con codec e ratio
tarc -t     archivio             Testa integrità (XXH64)
tarc -d     archivio  file...    Elimina file (wildcards supportati)
```

## Formato Archivio (.tar4)

```
[Header 13B] [chunk... chunk_end] ... [TOC]
```

- `Header`: magic `TARC`, versione, offset TOC
- Ogni file è diviso in chunk da 4MB compressi indipendentemente
- Ogni `Entry` nella TOC include il codec usato (1 byte)
- Hash XXH64 per verifica integrità su ogni file

## Novità v1.02 rispetto a v1.01

- Aggiunto supporto **LZ4** e **LZMA**
- **Selezione automatica codec** per tipo di file + analisi entropia runtime
- **Fallback automatico** a NONE se la compressione non conviene
- `Entry` estesa con campo `codec` (compatibilità binaria **non** mantenuta con v1.01)
- Refactoring in moduli: `engine`, `ui`, `license`, `io`
- Output `-l` mostra codec e ratio per ogni file
