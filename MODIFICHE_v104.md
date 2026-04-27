# 🛠 Changelog TARC v1.04 (Smart Update Edition)

Questa versione introduce un'intelligenza superiore nella gestione dei backup e una protezione totale della compatibilità binaria.

## 🚀 Novità Principali

### 1. Sistema "Smart Update" (Incrementale)
Il comando `-a` (append) è stato completamente riscritto. Ora il motore non si limita ad aggiungere file, ma esegue una comparazione intelligente:
- **Timestamp Tracking**: Il software memorizza la data di ultima modifica del file originale all'interno della TOC.
- **Deduplicazione Logica**: Se provi ad aggiungere un file già presente con lo stesso timestamp e dimensione, TARC lo salta istantaneamente, risparmiando il 100% del tempo di compressione.
- **Auto-Refresh**: Se il file su disco è più recente della versione in archivio, la vecchia entry viene invalidata e sostituita con la nuova.

### 2. Analisi dell'Entropia Real-time
Il `CodecSelector` ora include una fase di campionamento dinamico:
- **Entropy Sample**: Vengono analizzati i primi 4KB di ogni chunk.
- **Detection dei Dati Compresi**: Se l'entropia rilevata è superiore a **7.2**, il sistema identifica il dato come "già compresso" (es. file criptati o formati ignoti già ottimizzati) e forza il codec `NONE` o `LZ4` per evitare overhead inutile.

### 3. Retrocompatibilità v1.03
- **Legacy Header Support**: Il loader è ora in grado di rilevare archivi con `TARC_VERSION 103`.
- **Dynamic TOC Mapping**: Poiché la struttura `Entry` v1.04 include il nuovo campo `timestamp` (8 byte), il software mappa automaticamente le vecchie strutture mancanti di questo dato durante la lettura.

## 🔧 Dettagli Tecnici Build
- **Progetto**: Aggiornato a v1.04 nel file `CMakeLists.txt`.
- **Version Byte**: Incrementato a `104` in `types.h`.
- **Allineamento Binario**: Mantenuto tramite `#pragma pack(push, 1)` per cross-compatibility.
