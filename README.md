# TARC v1.03 — Hybrid Compression Archiver

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
| ZSTD  | File generici, binari, dati, **database**   | Bilanciato (default)  |
| LZMA  | Testo, codice sorgente (`.cpp`, `.json`)    | Ratio massimo         |
| NONE  | File ad alta entropia (rilevati runtime)    | Nessun overhead       |

La selezione avviene in tre fasi:
1. **Per estensione** — decisione rapida e deterministica
2. **Per categoria** — ottimizzazioni specifiche (es. database)
3. **Per entropia** — analisi dei primi 4KB per rilevare file già compressi a runtime

## 🆕 Supporto Database (v1.03)

TARC riconosce automaticamente i database Microsoft Access e applica ottimizzazioni specifiche:

### Estensioni Supportate
- `.mdb` — Microsoft Access Database (97-2003)
- `.accdb` — Microsoft Access Database (2007+)
- `.mde`, `.accde` — Database compilati/protetti
- `.mda`, `.mdw` — Add-in e Workgroup

### Ottimizzazioni Applicate
1. **Codec**: ZSTD (pattern ripetitivi compressi ottimamente)
2. **Livello boost**: +3 rispetto al livello base (massimizza ratio)
3. **Chunking**: 4MB per chunk (ottimale per grandi tabelle)
4. **Verifica**: XXH64 hash per integrità garantita

### Performance Tipiche
- **Ratio**: 60-85% di riduzione dimensione (dipende dalla complessità del database)
- **Velocità**: ~300-500 MB/s compressione, ~800-1200 MB/s decompressione
- **Integrità**: verifica automatica con hash XXH64

**Esempio d'uso:**
```bash
# Archivia database con livello 6 (boost automatico a 9 per .mdb)
tarc -c6 archivio.tar4 database.mdb

# Estrai e verifica integrità
tarc -t archivio.tar4
tarc -x archivio.tar4
```

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

## Changelog

### v1.05 (Brotli & 7-Zip Engine)
- 🚀 **Brotli Support**: Integrazione algoritmo Google per compressione testi superiore.
- 💎 **LZMA Ultra**: Ottimizzato il motore 7-Zip per file binari pesanti.
- 🤖 **Auto-best**: Il comando `-cbest` analizza ora se usare Brotli o LZMA in base all'estensione.
- 🔄 **Full Compatibility**: Pieno supporto in lettura per archivi v1.03 e v1.04.

### TARC v1.04 — Smart Hybrid Archiver

TARC (Tiny Advanced Resilient Compressor) è un archiver ibrido ad alte prestazioni che seleziona automaticamente il miglior codec in base al contenuto e allo stato del file.

## 🌟 Caratteristiche Speciali v1.04
- **Smart Update**: Backup incrementali ultra-veloci basati su timestamp e XXH64.
- **Hybrid Engine**: Selezione tra ZSTD, LZMA, LZ4 o NONE basata su estensione ed entropia.
- **Database Boost**: Ottimizzazione specifica per Microsoft Access (`.mdb`, `.accdb`) con incremento automatico del ratio.
- **Integrità Totale**: Verifica per ogni singolo file tramite hash XXH64.

## 📊 Algoritmo Ibrido

| Codec | Target Principale | Caratteristica |
|-------|-------------------|----------------|
| **LZMA** | Codice sorgente, Testo, SQL | Massimo Ratio (preset 0-9) |
| **ZSTD** | Database, Documenti, Binari | Bilanciato (livello 1-22) |
| **LZ4** | Immagini, Video, Log | Velocità estrema |
| **NONE** | Dati ad alta entropia | Pass-through (zero CPU) |

## 🛠 Utilizzo

### Creazione e Aggiornamento
```bash
# Crea un nuovo archivio (default livello 3)
tarc -c backup.tar4 ./progetto

# Aggiornamento intelligente (aggiunge solo file modificati)
tarc -a backup.tar4 ./progetto

### v1.03 (Database Edition)
- ✨ Aggiunto supporto ottimizzato per database `.mdb` e `.accdb`
- 🚀 Boost automatico livello ZSTD (+3) per file database
- 📊 Riconoscimento estensioni: `.mdb`, `.accdb`, `.mde`, `.accde`, `.mda`, `.mdw`
- 📖 Documentazione estesa per backup database Access

### v1.02
- Aggiunto supporto **LZ4** e **LZMA**
- **Selezione automatica codec** per tipo di file + analisi entropia runtime
- **Fallback automatico** a NONE se la compressione non conviene
- `Entry` estesa con campo `codec` (compatibilità binaria **non** mantenuta con v1.01)
- Refactoring in moduli: `engine`, `ui`, `license`, `io`
- Output `-l` mostra codec e ratio per ogni file

### v1.01
- Versione iniziale con supporto ZSTD base
