# GUI Development Plan — TARC

## Phase 0 — Libreria Condivisa (libtarc)

Prima della GUI, estrarre il core di TARC in una libreria statica/shared che la GUI possa linkare direttamente, senza invocare il CLI via shell.

### Tasks
- [ ] Creare target CMake `libtarc` (static + shared) che espone API pubbliche in `include/tarc/api.h`
- [ ] Definire API C per compatibilità con Swift e C#:

```c
typedef struct tarc_archive tarc_archive;

// Creazione archivio
tarc_archive* tarc_create(const char* output_path);
tarc_error    tarc_add_file(tarc_archive*, const char* path);
tarc_error    tarc_set_codec(tarc_archive*, const char* codec);
tarc_error    tarc_set_level(tarc_archive*, int level);
tarc_error    tarc_finalize(tarc_archive*);

// Estrazione
tarc_archive* tarc_open(const char* archive_path);
tarc_error    tarc_extract_all(tarc_archive*, const char* output_dir);
tarc_error    tarc_extract_pattern(tarc_archive*, const char* pattern, const char* output_dir);

// Lista
size_t        tarc_list_entries(tarc_archive*, tarc_entry** out);
void          tarc_free_entry_list(tarc_entry*, size_t count);

// Progress callback
typedef void (*tarc_progress_cb)(const char* file, size_t current, size_t total, void* userdata);
void  tarc_set_progress_cb(tarc_archive*, tarc_progress_cb cb, void* userdata);

// Error handling
const char*   tarc_error_message(tarc_error err);
void          tarc_free(tarc_archive*);
```

- [ ] Refactor `Engine` in classi interne, non esposte nell'API pubblica
- [ ] Aggiungere `tarc_set_progress_cb` per callback di progresso (utile per la barra nella GUI)
- [ ] Build: `make libtarc` produce `libtarc.dylib` (macOS) / `libtarc.dll` (Windows) / `libtarc.a`
- [ ] Test: collegare `tarc_test` alla libreria invece che compilare i sorgenti direttamente

### Deliverable
`libtarc.framework` (macOS) / `libtarc.dll + .lib` (Windows) con header C e doctest che passa 38/38.

---

## Phase 1 — macOS GUI (SwiftUI)

### Tech Stack
- **Linguaggio**: Swift 5.9+
- **UI Framework**: SwiftUI (macOS 14+)
- **Libreria**: `libtarc.framework` linkata via Bridging Header
- **Build**: Xcode project + CMake wrapper (o Package.swift con binary target)
- **Distribuzione**: Notarized .dmg via `spctl`

### Milestone 1.1 — Scheletro App
- [ ] Xcode project: `TARCStudio.app`
- [ ] Finestra principale con tab: **Compress** | **Extract** | **Browse**
- [ ] Drag & drop area per file/cartelle
- [ ] Integrazione `libtarc.framework` via bridging header
- [ ] App sandboxing (entitlements per lettura file)

### Milestone 1.2 — Funzionalità Compress
- [ ] Selezione file/folder (NSOpenPanel + drag & drop)
- [ ] Dropdown codec: Auto | LZMA2 | ZSTD | Brotli | LZ4 | STORE
- [ ] Slider/stepper livello compressione (1–19)
- [ ] Pulsante "Compress" → chiamata asincrona a `tarc_create` / `tarc_add_file` / `tarc_finalize`
- [ ] Progress bar con file corrente + ETA
- [ ] Salvataggio `.strk` con NSSavePanel
- [ ] Notifica al completamento (UNUserNotificationCenter)

### Milestone 1.3 — Funzionalità Extract
- [ ] Selezione archivio `.strk` (NSOpenPanel)
- [ ] Chiamata a `tarc_list_entries` → tabella con: filename, dimensione, ratio, codec
- [ ] Checkbox per selezionare/deselezionare file specifici
- [ ] Pattern filter (search bar)
- [ ] Pulsante "Extract" → NSOpenPanel per cartella destinazione → `tarc_extract_entries`
- [ ] Progress bar
- [ ] Apertura cartella nel Finder al termine

### Milestone 1.4 — Integrazione Sistema
- [ ] Quick Action nel Finder (Servicemenus) — "Compress with TARC"
- [ ] Associazione file `.strk` → TARCStudio
- [ ] Thumbnail/QuickLook plugin per `.strk`
- [ ] Menu bar icon con accesso rapido

### Milestone 1.5 — Polish & Distribuzione
- [ ] App icon (.icns)
- [ ] Crash reporter (Crashlytics / sentry)
- [ ] Code signing + notarization
- [ ] `.dmg` con background personalizzato
- [ ] Homebrew cask: `brew install --cask tarcstudio`

---

## Phase 2 — Windows GUI (C# / WPF)

### Tech Stack
- **Linguaggio**: C# 12
- **UI Framework**: WPF (.NET 8) con MVVM
- **Libreria**: `libtarc.dll` via P/Invoke
- **Build**: Visual Studio 2022 + CMake per libtarc.dll
- **Distribuzione**: MSI installer via WiX Toolset

### Milestone 2.1 — Scheletro App
- [ ] Progetto WPF: `TARCStudio`
- [ ] Main window con tab: **Compress** | **Extract** | **Browse**
- [ ] P/Invoke declarations per l'API C di libtarc
- [ ] Drag & drop (Windows Shell)
- [ ] ViewModel layer (CommunityToolkit.Mvvm)

### Milestone 2.2 — Funzionalità Compress
- [ ] Selezione file (OpenFileDialog + drag & drop)
- [ ] ComboBox codec: Auto | LZMA2 | ZSTD | Brotli | LZ4 | STORE
- [ ] Slider livello (1–19)
- [ ] Pulsante "Compress" → chiamata asincrona a libtarc via `Task.Run`
- [ ] ProgressBar + status text
- [ ] SaveFileDialog per output `.strk`
- [ ] Notification area toast al termine

### Milestone 2.3 — Funzionalità Extract
- [ ] OpenFileDialog per `.strk`
- [ ] ListView con colonne: Name, Size, Ratio, Codec
- [ ] Checkbox selezione + search filter
- [ ] Pulsante "Extract" → FolderBrowserDialog → `tarc_extract_entries`
- [ ] ProgressBar
- [ ] Apri cartella in Explorer al termine

### Milestone 2.4 — Integrazione Sistema
- [ ] Context menu "Compress with TARC" (registro Explorer)
- [ ] File association `.strk` con doppio click
- [ ] Thumbnail handler (COM) per `.strk`
- [ ] Eventuale UWP bridge per Windows Store

### Milestone 2.5 — Distribuzione
- [ ] MSI installer (WiX) con opzione PATH
- [ ] Code signing (Authenticode)
- [ ] Squirrel/Windows installer per auto-update
- [ ] Winget package

---

## Architecture Overview

```
┌───────────────────────┐     ┌───────────────────────┐
│    TARCStudio (macOS) │     │   TARCStudio (Windows) │
│    SwiftUI + libtarc  │     │    WPF + libtarc.dll   │
│    .dylib / .a        │     │    P/Invoke            │
└─────────┬─────────────┘     └──────────┬──────────────┘
          │                              │
          │    C API (tarc/api.h)        │
          └──────────────┬───────────────┘
                         │
               ┌─────────▼──────────┐
               │      libtarc       │
               │  Engine + I/O + UI │
               │  (C++17, static)   │
               └─────────┬──────────┘
                         │
               ┌─────────▼──────────┐
               │  LZMA  ZSTD  LZ4   │
               │  Brotli  xxHash    │
               └────────────────────┘
```

## Timeline (Stima)

| Phase | Durata | Dipendenze |
|-------|--------|------------|
| 0 — libtarc | 1 settimana | — |
| 1.1 — Scheletro macOS | 3 giorni | Phase 0 |
| 1.2 — Compress macOS | 1 settimana | 1.1 |
| 1.3 — Extract macOS | 1 settimana | 1.2 |
| 1.4 — Integrazione macOS | 3 giorni | 1.3 |
| 1.5 — Distribuzione macOS | 2 giorni | 1.4 |
| 2.1 — Scheletro Windows | 4 giorni | Phase 0 |
| 2.2 — Compress Windows | 1 settimana | 2.1 |
| 2.3 — Extract Windows | 1 settimana | 2.2 |
| 2.4 — Integrazione Windows | 4 giorni | 2.3 |
| 2.5 — Distribuzione Windows | 3 giorni | 2.4 |

**Totale: ~7-8 settimane** con una persona full-time.

## Rischi

| Rischio | Impatto | Mitigazione |
|---------|---------|-------------|
| API C non abbastanza espressiva | Alto | Progettare API con feedback dal GUI team prima di finalizzare |
| Progress callback su thread diverso | Medio | Documentare thread-safety; SwiftUI richiede `@MainActor` per UI updates |
| P/Invoke gestione memoria | Medio | Test automatizzati con struct/blittable types; usare `SafeHandle` |
| Notarizzazione macOS fallisce | Basso | Hardened Runtime + entitlements chiari; testare con `spctl` prima della release |
| Sandbox limita accesso file | Medio | App con `com.apple.security.files.user-selected.read-write` e `Downloads` entitlement |

---

## Prossimo Passo

Conferma la strategia e iniziare con **Phase 0 — libtarc**: refactoring della codebase per esportare l'API C pubblica.
