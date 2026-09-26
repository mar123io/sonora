# Sonora — Roadmap

> Shell desktop nativa in C++ che ospita una UI web in CEF, con audio engine nativo,
> integrazioni col sistema operativo, updater delta e pipeline di release completa.

**Obiettivo reale:** un portfolio project che dimostri le competenze richieste dalla squad
Desktop Natives di Spotify — C++ di produzione, codebase multi-linguaggio, integrazioni OS,
build/packaging/delivery, e un layer nativo che abilita una UI web.

| | |
|---|---|
| **Budget** | ~8-10 h/settimana × 13 settimane ≈ **115 ore** |
| **Piattaforma primaria** | Windows (sviluppo e test manuali) |
| **Piattaforma secondaria** | macOS — layer astratto + compilazione in CI, nessun test manuale |
| **Data di inizio** | 21 settembre 2026 |
| **Data di consegna (v1.0 + README)** | 20 dicembre 2026 |

---

## 1. Architettura di riferimento

Tre processi, come l'app desktop di Spotify:

```
┌─────────────────────────────────────────────────────────┐
│  sonora.exe  (browser process, C++20)                   │
│                                                          │
│  ┌────────────────┐   ┌──────────────────┐              │
│  │  sonora-core   │   │  platform/       │              │
│  │  audio engine  │   │  IPlatformMedia  │───► SMTC     │
│  │  library index │   │  IPlatformShell  │───► tray,    │
│  │  state machine │   │                  │     jumplist │
│  └───────┬────────┘   └──────────────────┘              │
│          │                                               │
│      sonora-bridge  (IPC tipizzato, capability versioned)│
│          │                                               │
│  ┌───────┴──────────────────────────────┐               │
│  │  CEF browser host                     │               │
│  └───────┬──────────────────────────────┘               │
└──────────┼───────────────────────────────────────────────┘
           │
   ┌───────┴────────┐          ┌──────────────────────────┐
   │ render process │          │ sonora-updater.exe       │
   │ sonora-ui (TS) │          │ delta patcher + rollback │
   │ sonora://app/  │          └──────────────────────────┘
   └────────────────┘
                                ┌──────────────────────────┐
                                │ sonora-releases (backend)│
                                │ manifest, delta, rollout │
                                └──────────────────────────┘
```

**Regola architetturale non negoziabile:** nessun `#ifdef _WIN32` fuori da `src/platform/`.
Ogni capability di piattaforma passa da un'interfaccia astratta con implementazioni separate
per file. È il singolo dettaglio che chi legge il repo userà per giudicare se sai progettare
codice cross-platform o solo scriverlo.

### Struttura del repo

```
Sonora/
├── CMakePresets.json
├── vcpkg.json
├── cmake/                    # FindCEF, toolchain, packaging helpers
├── src/
│   ├── core/                 # audio engine, library, playback state (no deps OS)
│   ├── bridge/               # protocollo IPC, generato da schema
│   ├── platform/
│   │   ├── iface/            # IPlatformMediaIntegration, IPlatformShell
│   │   ├── win/              # C++/WinRT: SMTC, tray, jumplist, toast
│   │   ├── mac/              # stub compilabile: MPNowPlayingInfoCenter
│   │   └── linux/            # stub: MPRIS (opzionale)
│   ├── shell/                # main, CEF app/client handlers, finestra
│   └── updater/              # client updater standalone
├── ui/                       # TypeScript + Vite, nessun framework pesante
├── schema/                   # bridge.schema.json + generatori
├── services/releases/        # backend manifest + delta
├── tools/bench/              # harness performance
├── tests/                    # Catch2
├── docs/
│   ├── adr/                  # architecture decision records
│   └── WRITEUP.md
└── .github/workflows/
```

### Stack

| Area | Scelta | Perché |
|---|---|---|
| Linguaggio | C++20 | Coroutine e `std::span` semplificano bridge e buffer |
| Build | CMake ≥ 3.28 + presets | Standard di fatto, leggibile da chiunque |
| Dipendenze | vcpkg in modalità manifest | Riproducibile, funziona in CI senza riti |
| Embedding web | CEF binary distribution (branch stabile) | È quello che usa Spotify |
| UI | TypeScript + Vite, Preact | Piccola, veloce, non distrae dal punto del progetto |
| Audio | miniaudio (backend WASAPI) | Header-only, controllo sul callback |
| Metadati | TagLib | De facto standard |
| Indice | SQLite | Zero setup, FTS5 per la ricerca |
| Test | Catch2 v3 | Integrazione CMake pulita |
| Windows API | C++/WinRT | SMTC richiede WinRT |
| Packaging | WiX v4 (MSI) + firma | Realistico, non uno zip |
| Crash | Crashpad | Quello che usa Chromium, quindi coerente con CEF |
| Delta | zstd `--patch-from` | Molto più semplice di bsdiff, risultati comparabili |
| Firma update | Ed25519 via libsodium | Piccolo, difficile da usare male |

---

## 2. Le 13 settimane

Ogni settimana ha un **deliverable verificabile**. Se a fine settimana non puoi dimostrare
il criterio di completamento, non passare oltre: recupera nella settimana successiva e
applica la lista di tagli (§4).

---

### FASE 0 — Il guscio

#### Settimana 1 — Scheletro buildabile (9h)

- [ ] `git init`, licenza MIT, `.gitignore`, `.clang-format` (stile Chromium), `.editorconfig`
- [ ] `CMakePresets.json` con preset `win-debug`, `win-release`, `mac-release`
- [ ] `vcpkg.json` manifest con le prime dipendenze (catch2, sqlite3)
- [ ] Script `cmake/DownloadCEF.cmake` che scarica e verifica lo SHA della distribuzione CEF
- [ ] Finestra Win32 nativa vuota: `WNDCLASSEX`, message loop, gestione DPI per-monitor v2
- [ ] Target `sonora_core` (static lib, zero dipendenze OS) + un test Catch2 che passa

**Completato quando:** su una macchina pulita, `cmake --preset win-debug && cmake --build --preset win-debug`
produce `sonora.exe` che apre una finestra ridimensionabile, e `ctest` è verde.

> ⚠️ Il download di CEF è il primo punto dove si perdono giornate. Se al terzo tentativo non
> compila, usa la distribuzione "Minimal" e rimanda l'ottimizzazione.

#### Settimana 2 — CEF dentro la finestra (9h)

- [ ] `SonoraApp : CefApp`, `SonoraClient : CefClient` con life-span e load handler
- [ ] Sottoprocesso separato per render/GPU (`sonora_helper.exe`), non single-process
- [ ] Custom scheme handler `sonora://app/` che serve il bundle UI da risorse embedded
      (niente `file://`: è la scelta giusta per sicurezza ed è un punto da citare nell'ADR)
- [ ] Setup UI: Vite + TypeScript, build che emette in `ui/dist`, embedding in CMake
- [ ] F12 apre i DevTools solo in build di debug

**Completato quando:** la finestra mostra una pagina servita da `sonora://app/index.html`,
i DevTools si aprono in debug e non in release, e chiudendo la finestra tutti i sottoprocessi
terminano (verificato in Task Manager).

**→ Commit taggato `v0.1-shell`.**

---

### FASE 1 — Il bridge nativo ↔ web

> Questa è la fase con più valore ingegneristico del progetto. Non affrettarla:
> è quella su cui riceverai le domande migliori in colloquio.

#### Settimana 3 — Protocollo tipizzato (9h)

- [ ] `schema/bridge.schema.json`: definizione dei metodi (nome, parametri, ritorno, versione
      minima di capability) e degli eventi push
- [ ] Generatore Python `tools/gen_bridge.py` che emette:
  - `src/bridge/generated/methods.h` (enum + struct di request/response)
  - `ui/src/bridge/generated.ts` (tipi + firme delle funzioni)
- [ ] Trasporto su `CefMessageRouter` (query asincrone con `Success`/`Failure`)
- [ ] Lato UI: `sonora.invoke<T>(method, params): Promise<T>` con timeout configurabile
- [ ] Il generatore gira come step CMake, non a mano

**Completato quando:** aggiungi un metodo nello schema, ricompili, e lo chiami dalla console
JS con autocompletamento dei tipi — senza aver scritto una riga di boilerplate.

#### Settimana 4 — Capability negotiation e robustezza (9h)

- [ ] `shell.getCapabilities()` restituisce `{ name, version }[]`; la UI costruisce un
      `CapabilitySet` e degrada quando manca qualcosa
- [ ] Un flag `SONORA_DISABLE_CAPS=media,tray` per simulare shell più vecchie a runtime
- [ ] Gestione errori: eccezione nativa → errore tipizzato lato JS, mai crash silenzioso
- [ ] Canale eventi push (nativo → UI) con coalescing: le posizioni di playback si mandano
      a 4 Hz, non a ogni callback audio
- [ ] Test Catch2 sulla serializzazione e sul dispatch; un test che verifica il degrado

**Completato quando:** lanci `SONORA_DISABLE_CAPS=media` e la UI nasconde i controlli media
senza errori in console; il metodo che tira un'eccezione produce una `Promise` rigettata
con messaggio leggibile.

**→ ADR #1: "Perché un bridge generato da schema invece di JSON ad-hoc".**
**→ Commit taggato `v0.2-bridge`.**

---

### FASE 2 — Il core audio

#### Settimana 5 — Suono dal primo file (10h)

- [ ] `AudioDevice` su miniaudio/WASAPI in shared mode, 48 kHz float32
- [ ] Decoder per FLAC, MP3, OGG (dr_libs o libsndfile via vcpkg)
- [ ] Ring buffer SPSC lock-free tra thread di decodifica e callback audio
- [ ] **Regola del callback:** nessuna allocazione, nessun lock, nessun I/O, nessun log.
      Scrivi questa regola in un commento in cima al file e rispettala.
- [ ] Contatore di underrun esposto come metrica

**Completato quando:** `sonora.exe --play percorso.flac` riproduce l'intero file con
0 underrun su 10 minuti.

#### Settimana 6 — State machine e controllo (9h)

- [ ] `PlaybackEngine` con stati espliciti: `Idle → Loading → Playing ⇄ Paused → Stopped`,
      transizioni testate in isolamento (il core non dipende da CEF né da WASAPI: iniettabile)
- [ ] Seek preciso, volume con rampa (niente click), coda con next/prev
- [ ] **Gapless:** pre-carica la traccia successiva quando mancano < 5 s e concatena senza
      riaprire il device
- [ ] Il tutto esposto via bridge: `player.play/pause/seek/setVolume/enqueue`
- [ ] Test del core con un device fittizio che "consuma" i campioni in tempo simulato

**Completato quando:** due tracce contigue di un album live passano senza buco udibile;
la suite del core gira in CI senza scheda audio.

#### Settimana 7 — Libreria e UI funzionante (10h)

- [ ] Scansione ricorsiva di una cartella, estrazione tag con TagLib su thread pool
- [ ] Indice SQLite (`tracks`, `albums`, `artists`) + FTS5 per la ricerca
- [ ] Scansione incrementale basata su mtime, watcher opzionale
- [ ] UI: sidebar, lista brani virtualizzata, player bar, coda, ricerca
- [ ] Copertine estratte dai tag e servite via `sonora://art/<hash>`

**Completato quando:** punti Sonora alla tua cartella Music, indicizza senza bloccare la UI,
e puoi cercare e riprodurre qualsiasi brano.

**→ Commit taggato `v0.3-player`. Da qui l'app è usabile davvero: usala come player quotidiano,
è il modo più veloce per trovare i bug che contano.**

---

### FASE 3 — Integrazioni con il sistema operativo

> Questa fase è il tuo differenziatore. Quasi nessun portfolio la contiene.

#### Settimana 8 — Media integration Windows (10h)

- [x] `IPlatformMediaIntegration` in `platform/iface/`: `setMetadata`, `setPlaybackState`,
      `setTimeline`, callback per i comandi in ingresso
- [x] Implementazione Windows con C++/WinRT: `SystemMediaTransportControls`
      — titolo, artista, album, thumbnail, stato, timeline
- [x] Comandi in ingresso: play, pause, next, previous, seek dal pannello di sistema
- [x] Media key hardware funzionanti anche con app in background
- [x] Stub macOS che **compila** in CI: `MPNowPlayingInfoCenter` + `MPRemoteCommandCenter`
      scritti ma non testati a mano — dichiaralo onestamente nel README

**Completato quando:** premi il tasto play/pause della tastiera con Sonora minimizzata e
funziona; l'overlay volume di Windows 11 mostra copertina, titolo e artista corretti.

**Verificato il 26 set 2026.** Il *nome* dell'applicazione sopra il pannello resta
"App sconosciuta" quando si lancia l'eseguibile dalla cartella di build: viene da un
collegamento registrato nel menu Start, non dal processo, quindi si chiude con l'MSI
della settimana 10. Da un collegamento dice "Sonora". Vedi README e diario.

#### Settimana 9 — Integrazione shell (8h)

- [ ] `IPlatformShell`: tray icon con menu contestuale (play/pausa/esci)
- [ ] Jump list con gli ultimi album riprodotti
- [ ] Thumbnail toolbar sui pulsanti della taskbar
- [ ] Single-instance: la seconda istanza passa gli argomenti alla prima e termina
- [ ] Protocol handler `sonora://track/<id>` registrato, apre l'app e riproduce
- [ ] Ripristino di posizione/dimensione finestra, gestione multi-monitor con DPI misti

**Completato quando:** doppio click su un `.sonora` link da browser apre l'app già in
esecuzione sulla traccia giusta; l'app si comporta bene spostandola tra due monitor a DPI diversi.

**→ ADR #2: "Come isolare le differenze di piattaforma senza #ifdef sparsi".**
**→ Commit taggato `v0.4-native`.**

---

### FASE 4 — Delivery, la parte che quasi nessuno fa

#### Settimana 10 — CI e packaging (9h)

- [ ] GitHub Actions, matrice:
  - `windows-latest`: configure, build, test, package, upload artifact
  - `macos-latest`: **solo compilazione** di core + bridge + platform/mac — dimostra che
    l'astrazione regge davvero, ed è verificabile da chiunque guardi i log
- [ ] Cache vcpkg e CEF (altrimenti ogni build costa 20 minuti)
- [ ] Installer MSI con WiX v4: shortcut, associazione file, uninstall pulita
- [ ] Versioning semantico derivato dal tag git, iniettato in CMake e nelle risorse dell'exe
- [ ] Firma del binario (certificato self-signed è accettabile: documenta che in produzione
      sarebbe un EV cert e perché)

**Completato quando:** un tag `v0.5.0` produce automaticamente una GitHub Release con l'MSI
firmato allegato, e la build macOS è verde.

#### Settimana 11 — Updater con delta e rollback (11h)

Il pezzo forte. Prenditi le ore extra qui.

- [ ] **Backend** `services/releases` (C++ con Drogon, oppure Go — scegli e motiva nell'ADR):
  - `GET /v1/manifest?channel=stable&version=X&platform=win-x64`
  - risponde con versione target, URL del full package, URL del delta da X, hash, firma
- [ ] **Generatore delta** in CI: `zstd --patch-from=vecchio.pkg nuovo.pkg` per ogni coppia
      di versioni recenti; misura e pubblica il rapporto di compressione
- [ ] **Client updater** (processo separato `sonora-updater.exe`):
  1. controlla il manifest all'avvio e ogni 6 h
  2. scarica il delta (fallback al full package se manca o fallisce)
  3. applica la patch in `%LOCALAPPDATA%\Sonora\staging`
  4. verifica hash **e firma Ed25519** prima di toccare qualunque cosa installata
  5. atomic swap alla chiusura dell'app
  6. **rollback:** la nuova versione deve scrivere un flag "avvio riuscito" entro 20 s;
     se al riavvio successivo il flag manca, l'updater ripristina la versione precedente
- [ ] Test end-to-end: installa 1.0.0 → server offre 1.0.1 → verifica che sia aggiornata
- [ ] Test negativo: delta corrotto → nessuna installazione, app resta funzionante
- [ ] Test negativo: build 1.0.1 che crasha subito → rollback automatico a 1.0.0

**Completato quando:** i tre test sopra girano in CI in un job dedicato.

#### Settimana 12 — Rollout, crash reporting, performance (10h)

- [ ] **Staged rollout:** il manifest include `rolloutPercent`; il client calcola un bucket
      stabile da un ID di installazione (hash) e si aggiorna solo se rientra. Un canale `beta`
      riceve sempre il 100%.
- [ ] **Crashpad:** handler fuori processo, upload dei minidump a un endpoint locale,
      upload dei simboli (PDB) da CI a ogni release, script che simbolizza un dump
- [ ] **Benchmark harness** `tools/bench`:
  - cold start (tempo fino al primo frame utile, misurato con ETW o timestamp interni)
  - RSS a riposo e durante playback
  - CPU media su 5 minuti di playback
  - tempo di scansione di una libreria di riferimento generata sinteticamente
- [ ] Job CI che fallisce se una metrica peggiora oltre il 10% rispetto alla baseline salvata
- [ ] **Una vera ottimizzazione:** profila l'avvio, trova il collo di bottiglia, sistemalo,
      e annota il prima/dopo. Questo numero vale più di tutto il resto del README.

**Completato quando:** apri un PR con una regressione volontaria e la CI la blocca.

**→ Commit taggato `v0.9-delivery`.**

---

### FASE 5 — Presentazione

#### Settimana 13 — Il repo che vuoi far leggere (9h)

Il progetto viene giudicato da questa settimana. Non comprimerla.

- [ ] **README.md** con, in quest'ordine:
  1. una frase che dice cosa è e perché esiste
  2. una GIF (10-15 s): app che parte, riproduce, appare nel pannello media di Windows
  3. il diagramma dell'architettura
  4. **la tabella delle performance**, con lo script per riprodurla
  5. la tabella dei delta update (dimensione full vs delta per 3 release reali)
  6. cosa è testato a mano e cosa solo compilato (onestà sul macOS)
  7. come buildare in 3 comandi
- [ ] **docs/WRITEUP.md** — 1500-2000 parole su **una** cosa difficile. Consiglio:
      *"Aggiornamenti delta con rollback automatico: cosa può andare storto quando aggiorni
      un'app che l'utente sta usando"*. In alternativa il thread audio lock-free.
- [ ] Rileggi i 2 ADR, aggiungine un terzo sul backend
- [ ] Release **v1.0.0** firmata con changelog
- [ ] Un issue aperto etichettato `good first issue` (segnala che pensi al progetto come
      qualcosa di vivo, non un compito consegnato)

**Completato quando:** una persona che non ti conosce capisce in 60 secondi di README cosa
hai costruito e quanto è difficile.

---

## 3. Cadenza settimanale suggerita

| Giorno | Ore | Cosa |
|---|---|---|
| Martedì sera | 2 | Il pezzo difficile della settimana, a mente fresca |
| Giovedì sera | 2 | Continuazione + test |
| Sabato mattina | 4-5 | Blocco lungo: la parte che richiede contesto (bridge, updater) |
| Domenica | 0.5 | Commit puliti, aggiorna il checkbox in questo file, nota cosa hai imparato |

Una riga di diario a settimana in `docs/JOURNAL.md`. A dicembre sarà il materiale grezzo
del writeup e delle risposte in colloquio.

---

## 4. Lista dei tagli (in ordine, se sei in ritardo)

Taglia dall'alto. **Non** toccare le fasi 1, 3 e 4: sono quelle che fanno il progetto.

1. **Settimana 9 interamente** — jump list, thumbnail toolbar, protocol handler. Tieni solo
   il tray e il single-instance (2h).
2. **Ricerca FTS5 e scansione incrementale** (settimana 7) — lista piatta e riscansione completa.
3. **Crash reporting** (settimana 12) — sostituisci con un handler minimale che scrive un
   minidump su disco, senza upload né simbolizzazione.
4. **Gapless** (settimana 6) — è la feature più costosa in rapporto a quanto si vede.
5. **Backend separato** (settimana 11) — servi il manifest da un JSON statico su GitHub Pages
   e documenta come sarebbe il servizio vero. Il delta e il rollback restano.

Se sei **in anticipo**, in coda c'è la Fase 6.

---

## 5. Fase 6 — Opzionale: agent integration (8-12h)

La job description cita esplicitamente "AI agent integrations within the Spotify Desktop
experience". Se hai tempo dopo la settimana 13:

- [ ] Esponi un sottoinsieme del bridge come **tool schema** (nomi, parametri JSON Schema,
      descrizioni) generato dallo stesso `bridge.schema.json`
- [ ] Un pannello in cui scrivi "metti qualcosa di tranquillo per lavorare" e l'agent chiama
      `library.search` + `player.enqueue`
- [ ] **Il punto interessante non è l'LLM, è il modello di permessi:** quali tool può invocare
      il layer web, come si autorizzano le azioni distruttive (svuota coda, elimina playlist),
      come impedisci che contenuto della pagina diventi istruzione. Scrivi un ADR su questo.
      È esattamente la conversazione che avresti in quel team.

---

## 6. Rischi noti

| Rischio | Probabilità | Mitigazione |
|---|---|---|
| CEF non compila / build lentissime | Alta | Distribuzione Minimal, cache CI aggressiva, timebox di 6h in settimana 1 |
| C++/WinRT ostile con CMake | Media | `cppwinrt` da vcpkg, non il tooling MSBuild; prototipo isolato prima di integrare |
| L'audio real-time consuma 3 settimane | Media | Il criterio è "0 underrun", non "qualità audiofila". Non inseguire la perfezione |
| Il layer macOS non compila mai in CI | Media | Aggiungi il job macOS già in settimana 1, così rompe presto e non a fine progetto |
| Il progetto diventa "un altro music player" | **Alta** | Il README deve parlare di shell, delta update e integrazioni OS. Il player è il pretesto |
| Scope creep sulla UI | Alta | La UI resta brutta e funzionale fino alla settimana 13. Non è quello che stai vendendo |

---

## 7. Cosa non fare

- **Non usare Qt, Electron o Tauri.** Tutto il valore del progetto sta nel codice nativo che
  eviteresti usandoli.
- **Non lasciare il README per ultimo.** Scrivi le prime tre righe in settimana 1 e aggiornale
  a ogni fase.
- **Non fare `git commit -m "wip"` per tre mesi.** La cronologia è parte del portfolio:
  messaggi in inglese, un commit = un cambiamento comprensibile.
- **Non nascondere i limiti.** "macOS: compilato in CI, non testato su hardware" ti fa
  guadagnare credibilità, non perderla.
- **Non aggiungere feature dopo la settimana 13.** Finito significa finito: candidati.

---

## 8. Checkpoint

| Data | Tag | Cosa deve esistere |
|---|---|---|
| 4 ott 2026 | `v0.1-shell` | Finestra nativa + CEF + UI servita da scheme custom |
| 18 ott 2026 | `v0.2-bridge` | IPC tipizzato generato da schema, capability versionate |
| 8 nov 2026 | `v0.3-player` | Player usabile sulla tua libreria |
| 22 nov 2026 | `v0.4-native` | SMTC, media key, tray, single-instance |
| 13 dic 2026 | `v0.9-delivery` | MSI firmato, delta update con rollback, CI con soglie perf |
| 20 dic 2026 | `v1.0.0` | README, writeup, 3 ADR, release pubblica |
