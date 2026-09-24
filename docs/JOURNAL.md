# Diario di bordo

Una riga a settimana. A dicembre questo file è il materiale grezzo per il
writeup e per le risposte in colloquio: cosa ho sbagliato, cosa mi ha sorpreso,
quanto tempo è costato davvero rispetto alla stima.

Formato: cosa ho fatto · cosa è costato più del previsto · cosa ho imparato.

---

## Settimana 1 — Scheletro buildabile

**Pianificata:** 21-27 set 2026 · **Effettiva:** 19-20 set 2026 (chiusa in anticipo)
**Stima:** 9 h · **Effettivo:** ___ h
**Tag:** `v0.1-shell`

### Obiettivo

`cmake --preset win-debug && cmake --build --preset win-debug` produce
`Sonora.exe`, si apre una finestra ridimensionabile, `ctest` è verde.

### Fatto

- [x] Configure e build su macchina pulita
- [x] La finestra si apre
- [x] `ctest --preset win-debug` verde
- [ ] Ridimensionamento e dimensione minima verificati a mano
- [ ] Spostamento tra due monitor a DPI diversi senza "salto"
- [ ] CI verde su tutti e tre i runner
- [x] `git tag v0.1-shell` (pushato)

Oltre a quanto previsto sono entrati anche due ADR, il diario e la CI
multipiattaforma, che la roadmap non collocava esplicitamente in settimana 1.

### Cosa è costato più del previsto

**Tutto il tempo perso è stato ambiente, zero C++.** Tre incidenti, in ordine:

1. **`VCPKG_ROOT` non impostata.** Il toolchain arrivava a CMake come
   `/scripts/buildsystems/vcpkg.cmake` — prefisso vuoto. `$env:VCPKG_ROOT` in
   PowerShell vive solo nella finestra corrente: serve
   `[Environment]::SetEnvironmentVariable(..., 'User')` e **un terminale nuovo**.

2. **Generatore fissato a `Visual Studio 17 2022` nel preset**, con VS 18 (2026)
   installato. `could not find any instance of Visual Studio` è un messaggio che
   suona come "non hai il compilatore" e invece significa "non hai *quella*
   versione". Risolto togliendo del tutto `generator` dai preset Windows: CMake
   sceglie la più recente che trova, e i preset non si rompono al prossimo
   aggiornamento del toolchain.

3. **La correzione non era arrivata sul disco.** Dopo aver sistemato il preset,
   l'errore era *identico*, parola per parola. Il file era stato riscritto con il
   contenuto vecchio. Un errore identico dopo una correzione non è il bug che
   resiste: è la correzione che non è stata applicata. Verificare prima di
   ri-lanciare, sempre.

### Cosa ho imparato

- **`CreateWindow` è una macro di `windows.h`.** Una funzione con quel nome viene
  riscritta in `CreateWindowW` appena qualcuno include l'header, con un errore di
  link incomprensibile. Il factory si chiama `CreateAppWindow`.
- **Per-monitor v2 va dichiarato nel manifest**, non con
  `SetProcessDpiAwarenessContext`: nel manifest è attivo prima che esista la
  prima finestra, quindi non si vede il re-layout all'avvio.
- **Il costo di setup è tutto all'inizio e non è codice.**
- **Il job macOS in CI dalla settimana 1 era la scelta giusta**: il layer Cocoa è
  scritto e compilato senza che io abbia un Mac.

### Aperto per la prossima settimana

- Verificare il comportamento DPI su due monitor a scala diversa.
- Prima esecuzione della CI.

---

## Settimana 2 — CEF dentro la finestra

**Pianificata:** 28 set - 4 ott 2026 · **Effettiva:** 20 set 2026
**Stima:** 9 h · **Effettivo:** ___ h
**Tag:** `v0.2-cef` — fase 0 completa

### Obiettivo

La finestra mostra una pagina servita da `sonora://app/index.html`, i DevTools si
aprono in debug e non in release, alla chiusura tutti i sottoprocessi terminano.

### Fatto

- [x] CEF 152 incapsulato, `libcef_dll_wrapper` compilato con `/MD`
- [x] Sottoprocesso helper separato (`sonora_helper.exe`)
- [x] Schema custom `sonora://app` con `CefResourceHandler`
- [x] La pagina carica e riporta `Origin: sonora://app`, `Secure context: true`
- [x] Bundle UI (Vite + TypeScript) incorporato nel binario, da disco in debug
- [x] Message pump esterno: un solo loop nativo nel processo
- [x] Asset store estratto in `src/assets/`, 9 test suoi, path traversal coperto
- [ ] F12 apre i DevTools in debug e non in release
- [x] Alla chiusura tutti i sottoprocessi terminano (Task Manager)
- [ ] CI verde
- [x] `git tag v0.2-cef` — creato in locale, **mai pushato**

### Cosa è costato più del previsto

**Il codice C++ contro l'API di CEF ha compilato al primo colpo.** `app.cpp`,
`client.cpp`, `runtime.cpp`: zero errori. Le firme erano state prese dagli header
upstream veri invece che dalla memoria, e si vede.

**Tutto il tempo è andato nell'integrazione col build system**, cinque
fallimenti in fila, nessuno dei quali era codice applicativo:

1. **`file(ARCHIVE_EXTRACT ... VERBOSE OFF)`** — `VERBOSE` è un flag booleano
   nudo, non una coppia chiave-valore, quindi `OFF` era un argomento orfano.
2. **`cef_paths.gypi` non esiste più.** Lo usavo come marcatore di estrazione
   completa; è stato rimosso quando CEF è passata a Bazel.
3. **`libcef_lib` non è un target.** `find_package(CEF)` definisce variabili e
   macro, nessun target: l'imported library la crea l'applicazione con
   `ADD_LOGICAL_TARGET`. Mancavano anche `${CEF_STANDARD_LIBS}` e
   `${CEF_LIBCEF_DLL_WRAPPER_PATH}`.
4. **La distribuzione minimal contiene solo i binari Release.**
   `cef_variables.cmake` punta `CEF_LIB_DEBUG` a `<root>/Debug/libcef.lib`
   incondizionatamente, e quella cartella non c'è.
5. **`/SUBSYSTEM:WINDOWS` senza `/ENTRY:wWinMainCRTStartup`.** Il default di quel
   subsystem è `WinMainCRTStartup`, la variante ANSI, che cerca `WinMain`; il
   layer platform definisce `wWinMain`. Subsystem ed entry point sono due
   impostazioni distinte. In un progetto VS normale non si vede perché MSBuild
   lo deduce da `CharacterSet=Unicode`.

Più un errore mio di refactor: spostando l'asset store in `sonora::assets` avevo
aggiornato le occorrenze di `AssetStore` ma non quelle di `Asset` nudo.

### Cosa ho imparato

- **Leggere gli script di build della dipendenza batte leggere la sua
  documentazione.** Tutti e cinque i problemi avevano la risposta dentro
  `cmake/cef_variables.cmake` e `cmake/cef_macros.cmake`, già sul disco. Ho perso
  due giri prima di aprirli.
- **Il *tipo* di distribuzione è una decisione architetturale, non un dettaglio
  di download.** Minimal significa "niente binari Debug", e da lì discende che
  una build Debug dell'app linka i binari Release di CEF. È sicuro perché
  `libcef.dll` espone un'API C e possiede le proprie allocazioni, quindi
  un'applicazione `/MDd` e una libreria `/MD` non condividono mai un heap.
- **Un ICE del compilatore (`C1907`) dopo una cascata di errori di tipo è un
  sintomo, non la causa.** Ho controllato tutte le occorrenze invece di fermarmi
  a quella segnalata, e infatti ce n'era una seconda che MSVC non aveva ancora
  raggiunto.
- **Le macro di CEF sovrascrivono, non accodano.**
  `SET_EXECUTABLE_TARGET_PROPERTIES` fa `set_property(... LINK_FLAGS ...)`: un
  `/MANIFEST:NO` messo prima sparisce. L'ordine delle chiamate è parte
  dell'interfaccia.
- **Silenziare un warning va fatto consapevolmente.** I 60 `LNK4199` vengono dai
  delay-load di CEF e non dicono nulla su questo codice; se li lascio, smetto di
  leggere l'output del linker, ed è così che poi sfugge quello vero.

### Da riprendere

- **La sandbox è disattivata** (ADR 0003). Va rimessa in settimana 10 quando il
  packaging fisserà un CRT unico per tutte le dipendenze. Scritto qui perché è
  esattamente il tipo di cosa che sopravvive silenziosamente fino alla release.
- Il codice macOS e Linux è cresciuto (entry point, paths, child window) senza
  che nessuno lo compili: i job CI non sono ancora mai girati.

---

## Settimana 3 — Protocollo tipizzato

**Pianificata:** 5-11 ott 2026 · **Effettiva:** 24 set 2026 (chiusa in anticipo)
**Stima:** 9 h · **Effettivo:** ___ h

### Obiettivo

Aggiungere un metodo in `schema/bridge.schema.json`, ricompilare, e chiamarlo
dalla console JS con i tipi completi — senza aver scritto una riga di
boilerplate.

### Fatto

- [x] `schema/bridge.schema.json` come unica sorgente di verità, `protocolVersion: 1`
- [x] `tools/gen_bridge.py` genera header C++, dispatch e TypeScript
- [x] Interfaccia handler generata **pure virtual**: un metodo nuovo rompe la build
- [x] `src/bridge/` senza alcuna dipendenza da CEF, testato anche su Linux/macOS
- [x] `CefMessageRouter` sui due lati, `sonoraQuery` / `sonoraQueryCancel`
- [x] `invoke()` tipizzato lato UI, con timeout a 10 s e cancellazione reale
- [x] Cinque righe diagnostiche in pagina, la quinta fallisce apposta
- [x] Build completa verde: `sonora_bridge`, `sonora_cef`, `sonora_helper`, `Sonora`, `sonora_tests`
- [x] La quinta riga riporta `code 3` e non un valore di memoria non inizializzata
- [ ] CI verde
- [ ] Committata (vedi "Da riprendere": non lo è ancora)

### Cosa è costato più del previsto

Di nuovo: **il protocollo e il generatore sono usciti al primo colpo**, 32 test
verdi. Il tempo è andato tutto su due problemi di linguaggio e uno di rumore.

1. **`C2027: utilizzo di tipo non definito 'SonoraRenderProcessHandler'`.**
   `app.h` tiene `CefRefPtr<>` di tipi dichiarati solo in avanti. Il distruttore
   implicito di `SonoraApp` chiama `Release()` su quei membri, e `Release()`
   vuole il tipo completo — quindi il distruttore veniva istanziato in ogni
   unità di traduzione che includeva l'header, e compilava o no a seconda di
   *cos'altro* quel file avesse incluso. Il codice non era sbagliato in un punto:
   era fragile ovunque. Risolto dichiarando `~SonoraApp()` e definendolo nel
   `.cpp`, dove i tipi sono completi.

2. **La riga di errore arrivava in pagina come `code -858993460`** con un
   messaggio di caratteri casuali. `-858993460` è `0xCDCDCDCD`: il riempimento
   MSVC per la memoria heap non inizializzata. Non un errore di logica, un
   oggetto letto prima di esistere.

   La causa era a due livelli di distanza. `SET_LIBRARY_TARGET_PROPERTIES(sonora_cef)`
   applica `CEF_COMPILER_DEFINES`, che contiene `_HAS_EXCEPTIONS=0` e `/GR-`
   senza alcun `/EH`. Quindi `cef/handlers.cpp` — che segnala gli errori
   lanciando `bridge::BridgeError` — veniva compilato con le eccezioni spente,
   mentre il `catch` vive in `sonora_bridge`, compilato normalmente. Lanciare
   attraverso quel confine è comportamento indefinito, e si comportava come tale.

   **Il percorso felice funzionava perfettamente.** Quattro righe verdi su
   cinque. Solo il ramo di errore attraversava il confine, ed è esattamente il
   ramo che si prova meno.

3. **Tolte le macro di CEF, sono tornate centinaia di `C4100`** dai suoi header,
   perché in quella lista di flag c'era anche `/wd4100` e le classi base di CEF
   sono piene di implementazioni di default che nominano parametri e non li
   usano. Risolto facendo entrare `${CEF_ROOT}` come header *esterni*
   (`/external:I` + `/external:W0`) invece che come normale include path — non
   spegnendo `C4100`, che su `cef/*.cpp` continua a dire qualcosa di vero.

   Due dettagli rendono la correzione fragile se fatta a metà: `${CEF_ROOT}`
   non deve restare *anche* fra gli include normali, perché `/I` viene cercato
   per primo e un header trovato lì non è esterno; e `/external:I` da solo non
   fa nulla, è `/external:W0` che abbassa il livello. Scritti a mano invece di
   usare la keyword `SYSTEM` di `target_include_directories`, la cui mappatura
   su `/external:` dipende da versione di CMake e generatore e che non avevo
   modo di verificare. Il modo di fallire è comunque leggibile: se i flag non
   arrivassero al compilatore la build si fermerebbe su `cannot open include
   file: 'include/cef_app.h'` invece di tornare silenziosamente rumorosa. La
   build successiva è passata, quindi ci sono arrivati.

### Cosa ho imparato

- **Le macro di build di una dipendenza codificano la politica di linguaggio
  della dipendenza.** Adottarle in blocco adotta silenziosamente quella politica
  anche per il proprio codice — qui: niente eccezioni, niente RTTI. È la stessa
  lezione della settimana 2 (`SET_EXECUTABLE_TARGET_PROPERTIES` sovrascrive
  `LINK_FLAGS`) portata al suo caso peggiore: lì perdevo un flag, qui cambiava
  il significato di `throw`.
- **I pattern di riempimento di MSVC sono informazione, non rumore.**
  `0xCDCDCDCD` heap non inizializzato, `0xCCCCCCCC` stack, `0xFEEEFEEE`
  liberato, `0xDDDDDDDD` cancellato. Riconoscere il valore ha portato dalla
  domanda sbagliata ("perché il codice di errore è negativo?") a quella giusta
  ("chi ha letto quell'oggetto prima che esistesse?").
- **Smettere di usare i flag di una dipendenza significa possederli tutti, uno
  per uno.** `/STACK:0x800000`, `/MANIFEST:NO`, `/SUBSYSTEM`, `/ENTRY`,
  `/wd4100`: tolta la macro, ognuno di questi va rimesso *sapendo perché*. La
  sequenza LNK4199 → C4100 è la stessa storia due volte, ed è la ragione per cui
  ogni soppressione in questo progetto ha un commento che dice cosa nasconde.
- **Un'interfaccia generata pure virtual è il modo più economico di rendere un
  contratto verificabile dal compilatore.** Aggiungere un metodo allo schema non
  produce un TODO: produce un errore di compilazione, subito, su entrambi i lati.
- **Il confine nativo↔web va trattato come non fidato anche quando la pagina è
  nostra.** `Request::Parse` controlla ogni campo. Un renderer è a una dipendenza
  compromessa di distanza dal poter mandare qualunque cosa.

### Da riprendere

- La quinta riga diagnostica è verde: `BridgeError` attraversa il confine con
  `code === 3` e il suo messaggio intatto. Fra "linka" e "si comporta" c'era una
  distanza reale — il binario precedente linkava benissimo e restituiva
  `0xCDCDCDCD`.
- **La settimana 3 non è mai stata committata**, e la settimana 4 è stata
  scritta sopra lo stesso working tree. Quando me ne sono accorto non esisteva
  più uno stato "fine settimana 3" da cui fare un commit: gli stessi file
  (`schema/`, `handlers.cpp`, `ui/src/main.ts`) contengono ormai entrambe le
  settimane. Committare a fine settimana non è disciplina per il gusto di
  esserlo — è ciò che rende una settimana una cosa separabile dalla successiva.
  Da qui in avanti: commit e tag *prima* di aprire la settimana dopo.
- Il tag `v0.2-cef` esiste in locale da due settimane e non è mai stato pushato.
  `git push` non porta i tag da solo, e questo è il tipo di dettaglio che si
  scopre quando qualcun altro clona il repo e non trova la storia che gli hai
  descritto.
- CI ancora mai eseguita. Terza settimana di fila che questa riga resta qui: i
  job macOS e Linux sono ipotesi, non verifiche. `docs/ci-workflow.yml` è ancora
  da spostare a mano in `.github/workflows/ci.yml`.
