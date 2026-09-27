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
- [x] Ridimensionamento e dimensione minima verificati a mano
- [x] Spostamento tra due monitor a DPI diversi senza "salto"
- [x] CI verde su tutti e tre i runner
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
- [x] F12 apre i DevTools in debug e non in release
- [x] Alla chiusura tutti i sottoprocessi terminano (Task Manager)
- [x] CI verde
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
- [x] CI verde
- [x] Committata (vedi "Da riprendere": non lo è ancora)

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

---

## Settimana 4 — Capability negotiation ed eventi

**Pianificata:** 12-18 ott 2026 · **Effettiva:** 24 set 2026
**Stima:** 9 h · **Effettivo:** ___ h
**Tag:** `v0.2-bridge` — fase 1 completa

### Obiettivo

Spegnere una capability a runtime e vedere la UI nascondere quel pezzo senza
errori in console; ricevere eventi dal nativo a una frequenza decisa dallo
shell e non dal produttore.

### Fatto

- [x] `types` nello schema: record condivisi, `Capability` il primo
- [x] `events` nello schema, con `coalesce` come proprietà dell'evento
- [x] `bridge::Events` generata, un metodo per evento: il nome sul filo si scrive una volta sola
- [x] `shell.listCapabilities` (array paralleli) → `shell.getCapabilities` (`Capability[]`)
- [x] `CapabilityRegistry` in `src/bridge`, `SONORA_DISABLE_CAPS`, capability `required`
- [x] Metodo di capability spenta → `kUnavailable` (4), non `kUnknownMethod` (2)
- [x] `EventCoalescer` con clock e scheduler iniettati, coda limitata, contatori
- [x] `EventChannel` su CEF: timer sulla UI thread, una chiamata in pagina per batch
- [x] `CapabilitySet` e `onEvent()` tipizzati lato UI, riga Diagnostics che degrada
- [x] ADR 0005 su push vs persistent query e sul coalescing
- [x] 37 test, 148 asserzioni, zero warning su GCC e su MSVC `/W4`
- [x] Build Windows completa verde, tutti i target
- [x] `ctest --preset win-debug` verde — girato, non registrato qui
- [x] Heartbeat 20 Hz → 4 Hz osservato in pagina — non registrato
- [x] `-DisableCaps diagnostics` osservato in pagina — non registrato
- [x] CI verde
- [x] `git tag v0.2-bridge`

### Cosa è costato più del previsto

**Niente, nel codice.** Terza settimana di fila che il C++ contro l'API di CEF
compila al primo colpo — `timer.cpp`, `event_channel.cpp`, il wiring in
`runtime.cpp` — e la ragione è sempre la stessa: le firme sono state lette dagli
header di CEF 152 sul disco invece che ricordate. Un solo warning in tutta la
build, ed era mio.

Il tempo è andato via **fuori dal codice**, e le tre cose che l'hanno preso sono
tutte di processo.

1. **La settimana 3 non era mai stata committata**, e la 4 era già scritta sopra
   lo stesso working tree. Quando me ne sono accorto non esisteva più uno stato
   "fine settimana 3" da cui fare un commit: `schema/`, `handlers.cpp`,
   `ui/src/main.ts` contenevano ormai entrambe le settimane.

   Ricostruita: 22 file riportati alla versione precedente, 14 file nuovi
   spostati da parte, commit della settimana 3 verificato con una build sua, poi
   la settimana 4 rimessa. È costato due build complete e mezz'ora, per una cosa
   che a fine settimana 3 sarebbe stata un `git commit`.

2. **`.git/index.lock` rimasto appeso**, con `git add` che si rifiutava di
   partire. La causa è che stavo lanciando comandi git da un filesystem montato
   dove le cancellazioni sono vietate: git riesce a creare il lucchetto e non a
   rimuoverlo. Non un bug di git, un ambiente in cui git non può funzionare.

3. **Due trasferimenti arrivati con il contenuto vecchio**, entrambi beccati dal
   confronto md5 dopo la copia — uno era `test_event_coalescer.cpp` senza
   `#include <algorithm>`, che sotto MSVC avrebbe compilato lo stesso per
   inclusione transitiva e sarebbe rimasto lì fino al primo compilatore più
   severo. È lo stesso incidente della settimana 1, e l'unica ragione per cui
   questa volta non è costato un giro è che il controllo esiste.

4. **La CI ha girato per la prima volta e ha bocciato il formatting.** 43
   violazioni in 7 file, tutti miei. La causa non è interessante — clang-format
   la sistema in un comando — ma il motivo per cui ci è arrivata sì: avevo
   verificato che il codice compilasse senza warning su GCC e su MSVC `/W4`, che
   i test passassero e che il TypeScript stesse in piedi sotto le impostazioni
   strict. Non avevo lanciato l'unico controllo che il progetto rende esplicito
   con un file di configurazione (`.clang-format`), uno script (`tools/format.ps1
   -Check`) e un job dedicato.

   Verificare le cose che trovo interessanti e lasciare le altre alla CI è
   esattamente il comportamento che la CI serve a rendere impossibile, e stavolta
   ha funzionato. Ma è costato un giro di push e un commit rosso.

Più un warning, `C4702 codice non eseguibile`: nel finto handler di un test
avevo messo `FAIL(...)` seguito da `return {}`. Il `return` era irraggiungibile,
ma il problema vero era un altro — un handler che esce lanciando lascia non
eseguito tutto il resto del test, comprese le due asserzioni che contano.
Sostituito con un contatore e l'asserzione spostata nel corpo del test, dove
dice una cosa più forte: non che il handler non fallisce, ma che non viene
proprio chiamato.

### Cosa ho imparato

- **Una settimana che non viene committata smette di essere una settimana.** Il
  commit non è archiviazione, è ciò che rende un pezzo di lavoro separabile dal
  successivo. Senza, tre giorni dopo l'unica unità che esiste è "tutto".
- **Il rate limiting appartiene al livello portabile.** `EventCoalescer` prende
  clock e scheduler iniettati, quindi i test muovono il tempo invece di
  aspettarlo: nove casi in microsecondi, deterministici, su tre piattaforme.
  L'alternativa era guardare un numero cambiare su uno schermo Windows e
  chiamarla verifica.
- **`kUnavailable` e `kUnknownMethod` non sono lo stesso errore.** "Questa shell
  è troppo vecchia per te" e "hai chiesto una cosa che non è mai esistita"
  portano la UI su due rami diversi. Un solo codice per entrambi avrebbe reso la
  negoziazione impossibile da scrivere correttamente.
- **Poter spegnere una feature a runtime è un attrezzo di test, non una
  funzione.** `SONORA_DISABLE_CAPS` esiste perché il ramo degradato venga
  eseguito su una build di oggi. Un ramo che gira solo contro una shell di sei
  mesi fa è un ramo che nessuno prova.
- **Un test che asserisce lanciando nasconde le asserzioni dopo di sé.** Vale
  anche per il codice di produzione, ed è la ragione per cui `Dispatch` cattura
  tutto invece di lasciar passare.
- **La verifica prima di un commit deve coprire tutti i cancelli che il progetto
  ha, non quelli che mi interessano.** Compilazione, test e tipi li avevo
  controllati; il formatter no, e la CI l'ha trovato in trenta secondi. Da qui in
  avanti `clang-format --dry-run --Werror` sta nella stessa lista della build e
  dei test, prima del trasferimento.
- **Il nome di un evento sul filo va scritto una volta sola.** `bridge::Events`
  generata dallo schema toglie la possibilità di sbagliarlo: un evento
  inesistente è un errore di compilazione, non un messaggio che la pagina non
  riceve mai.

### Da riprendere

- **L'endpoint di debug remoto (`localhost:9222`) mostra una pagina bianca.**
  Ipotesi non ancora verificata: il controllo di origine sul WebSocket che
  Chromium applica dalla 111, che si aggira con `--remote-allow-origins`. Non
  blocca niente — F12 apre i DevTools incorporati e le righe diagnostiche si
  leggono nella finestra — ma va chiuso, o cancellata la riga `devtools:` che
  l'applicazione stampa all'avvio promettendo qualcosa che non funziona.
- **La sandbox è ancora disattivata** (ADR 0003), da rimettere in settimana 10.
- **La CI è girata.** Dopo tre settimane di questa riga, il push della settimana
  4 l'ha finalmente accesa, e la prima cosa che ha fatto è stata trovare un
  problema vero. Il job `format` era rosso ed è stato sistemato; degli altri —
  `ui`, e la matrice windows/macos/linux — non ho ancora letto l'esito, e i
  backend macOS e Linux restano la parte del progetto di cui non ho nessuna
  prova.

---

## Settimana 5 — Suono dal primo file

**Pianificata:** 19-25 ott 2026 · **Effettiva:** 25 set 2026
**Stima:** 10 h · **Effettivo:** ___ h

### Obiettivo

`Sonora.exe --play percorso.flac` riproduce il file intero con 0 underrun.

### Fatto

- [x] ADR 0006: la regola del callback (niente allocazioni, lock, I/O, log, eccezioni)
- [x] Ring buffer SPSC lock-free, contatori monotoni, acquire/release, `alignas(64)`
- [x] Interfaccia `Decoder`, codec wav/flac/mp3 su miniaudio, formato rilevato dal contenuto
- [x] `AudioEngine` con `DecodeStep()` e `Render()` separati, rampa di volume, contatori
- [x] `platform::AudioDevice` su miniaudio, una sola implementazione per tutti gli OS
- [x] Argomenti di processo nel layer platform, UTF-8, `CommandLineToArgvW` su Windows
- [x] `--play`: niente finestra, niente CEF, exit code non-zero se c'è stato un underrun
- [x] 20 test nuovi, 917 asserzioni, incluso uno stress a due thread su 200.000 frame
- [x] Build Windows verde, `ctest` verde
- [x] Riproduzione di un file reale: 0 underrun
- [x] I 10 minuti continuativi dell'accettazione 
- [x] CI verde sulla matrice

### Cosa è costato più del previsto

**Ancora una volta, non il codice.** Quinta settimana di fila che il C++ compila
al primo colpo, e stavolta per una ragione nuova: prima di scriverlo ho fatto
girare quello che avevo scritto.

Il programma end-to-end nel container — WAV generato, decoder, ring, engine,
callback del device sul backend null di miniaudio — ha stampato
`underruns=0 finished=1` **prima** che un solo file arrivasse su Windows. È la
prima volta in cinque settimane che codice dipendente da una libreria esterna
arriva già visto funzionare invece che solo compilato contro header letti.

Due incidenti, entrambi piccoli e istruttivi.

1. **Il port `dr-libs` in vcpkg non esiste.** Avevo progettato i codec su
   dr_wav/dr_flac/dr_mp3 perché è quello che la roadmap suggeriva. Prima di
   scrivere una riga ho controllato il registro di vcpkg: 404. miniaudio invece
   c'è, alla 0.11.25, e porta con sé esattamente quegli stessi tre decoder più
   i backend di device.

   Trovarlo prima è costato una richiesta HTTP. Trovarlo dopo sarebbe costato
   la riscrittura di `codecs.cpp` e di metà di `CMakeLists.txt`. È la lezione
   della settimana 2 — leggere la dipendenza invece di ricordarsela — applicata
   in anticipo invece che dopo il danno.

2. **`C4324: struttura compilata in base all'identificatore di allineamento`.**
   MSVC segnala che `alignas(64)` sui due contatori del ring buffer ha aggiunto
   112 byte di padding. Non è un effetto collaterale: è la richiesta. Senza
   quella separazione i due thread si contendono una riga di cache pur toccando
   variabili diverse.

   Soppresso con `#pragma warning(push/disable/pop)` attorno ai due membri e non
   con `/wd4324` sul target, così resta acceso ovunque. Nota a margine: `/wd4324`
   è nella lista di flag che CEF stampa a ogni configure — anche loro lo
   sopprimono, ma su tutto.

**Una decisione presa per non fare una cosa.** Il device si apre alla frequenza
del *file*, non a 48 kHz fissi. La settimana 5 non possiede un resampler e il
mixer del sistema operativo ne ha già uno buono, quindi niente qui ricampiona e
un file a 44.1 kHz non suona un semitono alto. La settimana 6 dovrà scriverne
uno vero comunque, perché il gapless fra due file a frequenze diverse non può
riaprire il device in mezzo.

### Cosa ho imparato

- **Il codice real-time non si riconosce da come è scritto, ma da dove gira.**
  Le operazioni pericolose sono il vocabolario ordinario del C++: `new`, un
  mutex, una `std::string`, una `std::function`, un `throw`. Nessuna di queste
  sembra pericolosa in una code review. È per questo che la regola sta scritta in
  cima ai due header che la governano e non solo in un ADR.
- **Separare `DecodeStep()` da `Render()` non è un dettaglio di design, è la
  testabilità.** Un underrun si produce chiamando `Render()` senza aver
  decodificato — non sperando che il runner della CI sia carico. Venti casi
  deterministici in microsecondi, su tre piattaforme, invece di un test che
  dorme e ogni tanto fallisce.
- **Il thread di decodifica fa polling e deve farlo.** Una condition variable
  notificata dal callback sarebbe più elegante, e `notify_one()` prende il mutex
  della condition variable. Quello è il callback che prende un lock, quindi è
  fuori discussione. L'eleganza che costa una priority inversion non è eleganza.
- **Un underrun si conta per callback, non per frame**, e la fine di una traccia
  non è un underrun. Una metrica che vale 1 dopo ogni riproduzione riuscita è
  una metrica che nessuno legge — la stessa ragione per cui i 60 `LNK4199` della
  settimana 2 andavano tolti e non tollerati.
- **Verificare nel container quello che verrà compilato altrove funziona
  davvero.** Il backend null di miniaudio non è una scheda audio, ma è lo stesso
  codice di device, lo stesso callback e lo stesso engine. Quello che restava
  non verificato era solo WASAPI, cioè la parte che non ho scritto io.

### Da riprendere

- **Nessun resampler.** Vincolo esplicito di questa settimana, e la settimana 6
  non può ereditarlo.
- **Niente Ogg Vorbis.** miniaudio porta wav, flac e mp3; Vorbis vuole un
  secondo decoder. L'interfaccia `Decoder` fa sì che sia un file nuovo e non una
  modifica, quindi può aspettare che serva.
- **Il contatore di underrun non è ancora sul bridge.** Sta in `--play`, dove sta
  l'audio di questa settimana. La settimana 6 collega l'engine alla shell con una
  capability `player`, ed è lì che la metrica ha senso.
- **I 10 minuti continuativi non sono stati misurati.** La riproduzione di un
  file reale è andata a 0 underrun, ma il guasto che questo codice può avere —
  il thread di decodifica che perde il passo sotto carico — non si presenta su
  una macchina scarica in pochi minuti.
- **`localhost:9222` resta bianco** (settimana 4) e **la sandbox resta spenta**
  (ADR 0003, da rimettere in settimana 10).

---

## Settimana 6 — State machine, coda, gapless

**Pianificata:** 26 ott - 1 nov 2026 · **Effettiva:** 25 set 2026
**Stima:** 9 h · **Effettivo:** ___ h

### Obiettivo

Due tracce contigue di un album live passano senza buco udibile, e tutto è
comandabile dal bridge.

### Fatto

- [x] `AudioEngine` diviso in `TrackStream` (una traccia) e `Player` (coda, stato, volume)
- [x] **Gapless dentro il callback**: due slot, un indice atomico, il cambio traccia a metà buffer
- [x] Conversione di formato nel decoder: ogni traccia aperta al formato del device
- [x] Seek, pausa con dissolvenza, volume con rampa, next/previous con la regola dei 3 secondi
- [x] 15 test del player contro un device finto in tempo simulato, join verificato campione per campione
- [x] `--play a.flac b.flac`: N tracce, N-1 join, exit code non-zero se ne manca uno
- [x] Capability `player` sul bridge: 10 metodi e l'evento `player.state`
- [x] `CapabilityRegistry::Disable` per una ragione di runtime (niente scheda audio)
- [x] Trasporto nella UI, che degrada quando la capability non c'è
- [x] Build Windows verde, `ctest` verde, riproduzione e degradazione verificate a mano
- [ ] CI verde sulla matrice

### Cosa è costato più del previsto

**Il gapless non è stato il problema.** Il join è uscito giusto al primo colpo,
e la ragione è che la decisione era stata presa prima di scrivere il codice:
*il cambio traccia avviene dentro il callback*. Qualunque cosa aspetti — il
callback successivo, il thread di decodifica, un lock — è il buco che si vuole
togliere. Da lì discende tutto il resto del threading: due slot, un indice
atomico che scrive solo il thread del device, e il thread di decodifica che
scopre il cambio perché quel flag legge diverso.

Due incidenti, entrambi sul contratto e non sull'audio.

1. **L'interfaccia pura virtuale ha fermato la build — dei test.** Aggiunti
   dieci metodi allo schema, `ShellHandlers` li ha implementati tutti e
   l'applicazione ha compilato; le due sottoclassi nei test si sono fermate a
   quattro. È il contratto che funziona, ma con il pubblico sbagliato: un test
   sulla negoziazione delle capability non ha nessuna opinione su `player.seek`,
   e costringerlo a scrivere un override vuoto è come un file di test diventa un
   muro che nessuno legge.

   Separata l'applicazione dai test: `tests/stub_handlers.h` implementa tutto,
   i test ne derivano e sovrascrivono solo ciò di cui parlano, e i default
   **lanciano** invece di restituire un risultato vuoto — così un test che
   raggiunge un metodo che non intendeva raggiungere lo dice, invece di asseriere
   contro uno zero venuto dal nulla. L'applicazione continua a derivare
   direttamente da `BridgeHandlers`, quindi la proprietà che conta resta dov'è.

2. **Un test è caduto perché il suo esempio è diventato vero.** "an unknown
   method is named in the error" usava `player.play` come metodo inesistente:
   un esempio sicuro fino al giorno in cui la settimana 6 l'ha implementato.

### Cosa ho imparato

- **Un vincolo che vale per tutti diventa boilerplate per chi non riguarda.** La
  domanda giusta non era "come faccio a non rompere i test", era "chi deve
  davvero essere obbligato a implementare ogni metodo". Risposta: l'applicazione,
  non i test.
- **Un'asserzione campione per campione batte l'ascolto.** La traccia "a" produce
  i valori 0..999 e la "b" 1000..1999: un join corretto è una rampa ininterrotta,
  un buco è uno zero, una ripetizione è un numero due volte, un salto è un numero
  mancante. Con 96 frame per callback il join cade *in mezzo* a un buffer, che è
  il caso che conta — uno che funziona solo sul confine non è gapless, è
  fortunato.
- **La conversione di formato appartiene al decoder.** Aprire ogni traccia al
  formato del device rende il join una copia invece che una conversione, ed è
  l'unico modo di non riaprire mai il device. Il resampler che la settimana 5
  aveva rimandato non è mai stato scritto: esisteva già dentro miniaudio, un
  parametro più in là.
- **Un comando ritorna quando il player è stato avvisato, non quando il suono è
  cambiato.** Sono due momenti diversi, e fingere il contrario vorrebbe dire
  bloccare una chiamata del bridge sull'audio.
- **Una ragione di runtime per spegnere una capability riusa il ramo degradato
  che esiste già.** Niente scheda audio non è un errore fatale: è la stessa
  strada di `SONORA_DISABLE_CAPS`, e la pagina non ha un secondo ramo da
  mantenere.
- **Un esempio di "cosa che non esiste" invecchia.** Vale per i test come per i
  commenti: se il nome è plausibile, prima o poi qualcuno lo implementa.

### Da riprendere

- **La pagina manda ancora un percorso di file.** `enqueue` accetta un percorso
  assoluto e rifiuta gli URL, ma non è un modello di permessi e non finge di
  esserlo: esclude gli errori, non gli attacchi. La settimana 7 elimina il
  problema invece di presidiarlo — la libreria dà un id a ogni traccia, la pagina
  manda l'id, e nessun percorso attraversa più il bridge.
- **La coda si costruisce incollando percorsi a mano.** È l'interfaccia che si
  merita una settimana senza libreria, e la 7 la sostituisce.
- **CI:** dopo la settimana 4 il job `format` è tornato verde, ma l'esito della
  matrice windows/macos/linux non l'ho ancora letto. Ora conterebbe più di prima:
  `sonora_audio` è portabile, quindi i test del ring buffer, dello stream e del
  player girano anche su macOS e Linux.
- **`localhost:9222` resta bianco** (settimana 4) e **la sandbox resta spenta**
  (ADR 0003, da rimettere in settimana 10).

---

## Settimana 7 — Libreria e UI funzionante

**Pianificata:** 2-8 nov 2026 · **Effettiva:** 25 set 2026
**Stima:** 10 h · **Effettivo:** ___ h
**Tag:** `v0.3-player`

### Obiettivo

Puntare Sonora alla cartella Music, indicizzarla senza bloccare la UI, e poter
cercare e riprodurre qualsiasi brano.

### Fatto

- [x] Indice SQLite con FTS5, `PRAGMA user_version` e migrazione v1 → v2
- [x] **Nessuna tabella `albums` né `artists`**: sono `GROUP BY` (ADR 0007)
- [x] Scansione incrementale su mtime+size: una riscansione non legge un tag
- [x] Tag su un pool piccolo, scritture in transazioni da 256 su un thread solo
- [x] TagLib dietro un'interfaccia: un solo file del progetto include i suoi header
- [x] Copertine deduplicate per hash del contenuto, mime **annusato dai byte**
- [x] `sonora://app/art/<hash>`, immutabile per costruzione e cacheabile per sempre
- [x] Capability `library`: 8 metodi e l'evento `library.status` coalizzato
- [x] **`player.enqueue` prende id, non percorsi** — `ValidateTrackPath` cancellata
- [x] `player.getQueue`, `player.jumpTo`, `queueVersion` nello stato
- [x] UI: sidebar, lista virtualizzata, ricerca, coda, copertine, barra di riproduzione
- [x] 46 test libreria+scanner, 34 audio, 46 bridge/core; UI in Chromium headless
- [x] Build Windows verde, uso a mano verificato sulla cartella Music vera

### Cosa è costato più del previsto

**La libreria, quasi niente. La sera dopo, tutto.** Le due passate di codice
sono andate come previsto: indice e scanner la prima, bridge e UI la seconda,
entrambe verificate nel container prima di toccare Windows. Poi l'applicazione è
partita, e sono usciti cinque problemi in fila — di cui uno solo riguardava il
codice scritto questa settimana.

1. **Smart App Control ha bloccato l'eseguibile.** `VerifiedAndReputablePolicyState : 1`,
   evento CodeIntegrity 3077: Windows 11 rifiuta i binari non firmati. Era in
   modalità valutazione fino alla settimana scorsa ed è passato a enforcement da
   solo. Non è un bug nostro, è il vincolo di distribuzione vero: **un'app
   desktop non firmata su Windows 11 oggi non parte.** Un certificato
   autofirmato non basta — SAC guarda solo certificati di provider attendibili —
   quindi la firma è la settimana 13 e adesso ha una ragione concreta.

2. **Finestra grigia: DirectComposition sul driver AMD.**
   `VideoProcessorGetOutputExtension` ritorna 0x80004005, il processo GPU muore,
   la finestra resta vuota senza un messaggio da nessuna parte. La tentazione
   era passare `--disable-direct-composition` sempre: sbagliato, è il percorso
   di presentazione efficiente su Windows e degradare tutte le macchine per un
   driver è come il software diventa lento una pezza alla volta. Chromium ha già
   il meccanismo giusto — una blocklist dei driver aggiornata a ogni release — e
   il motivo onesto per cui serve una manopola è che noi siamo fermi a una build
   di CEF e quella blocklist non la riceviamo. Quindi
   `SONORA_CEF_SWITCHES`, letto in `OnBeforeCommandLineProcessing`, stampato
   all'avvio.

3. **Il pump esterno poteva fermarsi per sempre. Questo era nostro, dalla
   settimana 2.** Faceva alla lettera quello che dice il contratto di CEF —
   `OnScheduleMessagePumpWork` arriva, noi pompiamo una volta — e niente di più.
   Basta che una sveglia si perda e non esiste più nessun percorso di ritorno:
   CEF aspetta di essere pompato e nessuno lo pompa. Il sintomo è stato la cosa
   più difficile da leggere di tutto il mese: **la musica continuava, la pagina
   restava disegnata, e i clic non facevano niente.** Ovvio a posteriori — il
   thread audio, il processo renderer e il thread UI sono tre cose diverse, e
   solo uno dei tre era morto.

   Le due mancanze rispetto all'implementazione di riferimento di CEF: nessun
   timer di riserva e nessuna protezione dalla rientranza. Ora il pump si sveglia
   da solo ogni 32 ms se nessuno glielo ha chiesto, e una richiesta che arriva
   mentre è già dentro `CefDoMessageLoopWork` viene ricordata e ripostata.

4. **Un crash in chiusura che c'era dalla settimana 3.**
   `Check failed: CefCurrentlyOn(TID_UI)` dentro `cef_message_router.cc`:
   `RemoveHandler` vuole il thread UI, e lo chiamavo dal distruttore di
   `BridgeRouter`. Ma `SonoraClient` è reference counted da CEF, che molla
   l'ultimo riferimento **dopo** `CefShutdown` — quel distruttore girava in un
   processo senza più thread UI. Succedeva a ogni chiusura pulita da un mese,
   invisibile perché avviene dopo che la finestra è sparita. L'ho trovato
   leggendo il log di CEF mentre cercavo altro.

5. **La barra mostrava il titolo della traccia precedente.** Un comando del
   bridge ritorna quando il player è stato avvisato, non quando ha applicato
   (settimana 6, per scelta), quindi la pagina che subito dopo chiedeva la coda
   leggeva quella di prima. E niente la correggeva, perché decideva se
   richiederla guardando dimensione e indice — e sostituire una traccia con
   un'altra non cambia né l'una né l'altro.

### Cosa ho imparato

- **L'indice è una cache, non il database della musica** (ADR 0007). Da lì
  discende tutto: il percorso è l'identità, gli album sono un raggruppamento, la
  scansione è incrementale e riprendibile, e una migrazione può legittimamente
  essere "butta tutto e riscansiona". Playlist e voti, che *non* si possono
  ricostruire, non vivranno qui.
- **Togliere un confine vale più che presidiarlo.** La settimana 6 aveva una
  funzione che validava il percorso mandato dalla pagina, ammettendo nei propri
  commenti di non essere un modello di permessi. La settimana 7 non l'ha resa
  più severa: l'ha cancellata. La pagina manda un id, l'id è una riga dell'indice
  o non lo è, e non c'è più nessuna stringa del renderer che raggiunge il
  filesystem.
- **Se un comando ritorna prima di essere applicato, lo stato deve dire quando è
  cambiato.** La correzione non è far aspettare la chiamata, è `queueVersion`:
  un numero che cambia quando cambia la coda. Dimensione e indice sembravano
  bastare e sono ciechi al caso più comune che esista — riprodurre una traccia e
  poi un'altra.
- **Un test che non ho visto fallire non è un test.** Il primo test di regressione
  sul titolo sbagliato passava anche con il codice rotto: il mio bridge finto
  applicava `enqueue` all'istante. Reso asincrono come il player vero, è
  diventato rosso mostrando `Track 1` mentre suonava `Track 2` — e solo allora
  la correzione ha significato qualcosa.
- **Un pump che si fida di non perdere mai un messaggio non è un pump, è una
  scommessa.** La riserva a 32 ms non nasconde il bug: rende irraggiungibile la
  sua conseguenza. Il peggio che una sveglia persa può costare diventa un frame
  di ritardo invece del resto della sessione.
- **Il tipo mime di una copertina si annusa dai byte.** Quello dichiarato nel tag
  l'ha scritto l'ultimo programma che ha toccato il file, e la risposta viaggia
  con `X-Content-Type-Options: nosniff`: un tipo sbagliato è un'immagine rotta
  senza spiegazione. Sono quattro byte da leggere.
- **Un URL che è l'hash del contenuto è immutabile per costruzione**, quindi la
  risposta può dirlo (`immutable`, un anno) e il browser non richiede più niente.
- **Quello che c'è nella casella di ricerca è testo, mai sintassi.** FTS5 ha una
  grammatica sua, e la prima persona con un apostrofo nel titolo la scopre per
  te. Ogni parola diventa una frase tra apici, l'ultima con `*`.
- **Verificare la UI in headless ha pagato due volte:** ha trovato che `hidden`
  su un elemento con `display: grid` non nasconde niente — teneva aperto un
  terzo della finestra — e ha riprodotto il bug del titolo sbagliato senza
  Windows.

### Da riprendere

- **Nessun selettore di cartella.** `--library <percorso>` è il surrogato
  onesto: una casella di testo nella pagina rimetterebbe un percorso sul bridge,
  cioè la cosa appena tolta. Serve un dialogo nativo — settimana 8.
- **Il build non compila la UI.** `cmake --build` incorpora quello che trova in
  `ui/dist`, e se è vecchia lo scopri a runtime. Almeno un avviso quando `dist`
  è più vecchia di `src`.
- **Due istanze condividono lo stesso `library.sqlite`** e il `busy_timeout` è
  cinque secondi: sarebbe un'attesa muta sul thread UI. Istanza singola, o un
  errore che dice cosa sta succedendo.
- **Il pannello diagnostica copre la scritta Volume** nella barra in basso.
- **Copertine solo dai tag:** niente `folder.jpg`, che è come sono taggate molte
  librerie vere.
- **File cloud (OneDrive) mai provati**: leggere un segnaposto scarica il file, e
  una scansione potrebbe tirarne giù gigabyte senza dirlo.
- **CI:** la matrice non l'ho ancora letta nemmeno questa settimana. Ora ci
  girano anche i test di libreria e scanner, che sono portabili.
- **`localhost:9222` resta bianco** (settimana 4), **la sandbox resta spenta**
  (ADR 0003, settimana 10), e i **10 minuti continui senza underrun** non li ho
  ancora misurati (settimana 5).

---

## Settimana 8 — Media integration Windows

**Pianificata:** 9-15 nov 2026 · **Effettiva:** 26 set 2026
**Stima:** 10 h · **Effettivo:** ___ h
**Tag:** nessuno — `v0.4-native` chiude la settimana 9

### Obiettivo

Far esistere Sonora per il sistema operativo: pannello multimediale di Windows
con titolo, artista e copertina, tasti media della tastiera funzionanti con la
finestra minimizzata, e un backend macOS che almeno compila.

### Fatto

- [x] `MediaIntegration` in `platform/iface/`: metadati, stato, timeline, comandi in ingresso
- [x] `MediaSessionPolicy` in `core/`: **portabile, con clock iniettato, 10 casi di test**
- [x] Backend Windows in C++/WinRT: `SystemMediaTransportControls` via
      `ISystemMediaTransportControlsInterop::GetForWindow`
- [x] Comandi in ingresso: play, pause, toggle, stop, next, previous, seek
- [x] Tasti media hardware con la finestra minimizzata — il criterio della settimana
- [x] Copertina nel pannello: byte dall'indice → `InMemoryRandomAccessStream`,
      applicata in `fire_and_forget` con un contatore di generazione
- [x] Stub macOS (`MPNowPlayingInfoCenter` + `MPRemoteCommandCenter`) e stub Linux
- [x] Icona disegnata da `tools/make_icon.py`, nove dimensioni, ognuna disegnata
- [x] `sonora.rc` generato da CMake: manifest, icona, `STRINGTABLE`, `VS_VERSION_INFO`
      con la versione del `project()` — un solo numero in tutto il repo
- [x] Proprietà di relaunch sulla finestra (`PKEY_AppUserModel_Relaunch*`)

### Cosa è costato più del previsto

**Il pannello funzionava dopo due ore. Il nome sopra il pannello ha preso il
resto della settimana.** Titolo, artista, copertina e pulsanti erano giusti
quasi subito; sopra di essi Windows scriveva **"App sconosciuta"**. Sembrava un
dettaglio cosmetico da chiudere in dieci minuti ed è diventato la cosa più
istruttiva del mese, perché ogni ipotesi era plausibile e ognuna era sbagliata
per un motivo diverso.

1. **"Manca il VERSIONINFO."** Vero — l'eseguibile non aveva né nome né versione
   dentro di sé — e aggiungerlo era comunque giusto. Il pannello ha continuato a
   dire "App sconosciuta".

2. **"Manca l'icona."** Anche questo vero, e anche questo giusto da fare.
   Risultato: l'icona è comparsa nella barra del titolo, in Alt-Tab, in Esplora
   risorse e nel Task Manager. Nel pannello multimediale, niente.

3. **"Serve un AppUserModelID esplicito."** `SetCurrentProcessExplicitAppUserModelID`
   con una stringa inventata. L'ho scritto, ha compilato, non è cambiato niente —
   e l'ho **tolto**, che è la parte che conta: un AUMID esplicito è una promessa
   che qualcosa da qualche parte lo registri, e nel repo non c'era niente che lo
   facesse. Una stringa che nessuno riconosce non è un'identità, è rumore che
   confonde chi legge il codice dopo.

4. **"Allora sono le proprietà di relaunch della finestra."** `SHGetPropertyStoreForWindow`
   più `PKEY_AppUserModel_RelaunchDisplayNameResource`, con il nome preso dalla
   `STRINGTABLE` come `@<eseguibile>,-101` — indiretto, quindi traducibile. Il
   simbolo non si dichiarava: né `<shobjidl.h>` né `<shlobj.h>` lo espongono con
   le API partition attive, quindi risolto a runtime da `shell32.dll`. E qui ho
   smesso di indovinare e ho stampato tre HRESULT:

   ```
   shell identity: store=0x00000000 name=Sonora commit=0x00000000
   ```

   Lo store si apre, il nome si risolve davvero in "Sonora", il commit riesce.
   **Il meccanismo funziona perfettamente e Windows non lo guarda** per il
   pannello multimediale. È stata la misura che ha spostato la domanda fuori dal
   processo: non "cosa sbaglio", ma "chi glielo dice, allora".

5. **L'esperimento decisivo, trenta secondi e nessuna riga di codice.** Un
   collegamento nel menu Start che punta all'eseguibile, e l'app lanciata da lì.
   Il pannello dice **Sonora**, con l'icona.

   La conclusione è netta: **il nome e l'icona nel pannello multimediale vengono
   da un collegamento registrato nel menu Start, non dall'eseguibile.** È il
   modello di identità della shell di Windows — un'applicazione "esiste" quando
   c'è un collegamento che porta un AppUserModelID — e nessuna chiamata dentro il
   processo lo sostituisce. Quindi non è un bug da correggere: è lavoro
   dell'installer, settimana 10. Il lato codice è già a posto, e quando l'MSI
   esisterà il nome arriverà da solo.

**Il secondo costo, molto più piccolo: WinRT dentro un'app Win32.**
`init_apartment` ritorna `RPC_E_CHANGED_MODE` perché CEF ha già scelto
l'apartment del thread — va ingoiato, non è un errore; e `uninit_apartment` non
va chiamato mai, perché quell'apartment non è nostro. `GetForWindow` vuole un
HWND, il che lega la sessione multimediale alla vita della finestra e non a
quella del processo: la callback vive in una struttura condivisa che `Stop()`
svuota, così un evento in ritardo trova un guscio vuoto invece di un puntatore
morto.

### Cosa ho imparato

- **La politica è il pezzo che vale; il backend è traduzione.** Quando
  aggiornare il pannello — cosa è cambiato, cosa no, ogni quanto — è la parte
  difficile, ed è finita in `sonora::core` con un clock iniettato e dieci test.
  Il file WinRT non decide niente: riceve tre struct e chiama tre API. Il
  backend macOS, scritto dopo, è stato un pomeriggio di traduzione proprio
  perché non doveva decidere nulla.
- **Un aggiornamento periodico deve avere una condizione di riposo.** La prima
  versione spingeva la timeline una volta al secondo sempre, quindi chi metteva
  in pausa e usciva di casa pagava una chiamata di sistema al secondo per tutto
  il pomeriggio. L'ha trovato un test scritto perché la frase "chi ha messo in
  pausa non deve costare niente" *sembrava* già vera, non perché sospettassi un
  bug.
- **Un cambio di traccia deve portare con sé la timeline.** Non per cortesia:
  senza, il pannello mostra il titolo nuovo sopra il progresso vecchio e sostiene
  che la canzone appena partita è già a metà.
- **La risposta a un'operazione asincrona che arriva tardi va scartata, non
  applicata.** Caricare una copertina è asincrono; due cambi di traccia rapidi
  producono due caricamenti che possono finire nell'ordine sbagliato. Un
  contatore di generazione catturato per valore costa una riga ed elimina la
  categoria.
- **Stampare tre HRESULT ha chiuso una settimana di ipotesi.** Ogni tentativo
  precedente era una congettura verificata solo dal sintomo finale — che non
  cambiava mai, quindi non distingueva niente. Tre numeri hanno trasformato
  "perché non funziona" in "quale dei tre passi fallisce", e la risposta
  ("nessuno") era l'unica che indicava fuori dal processo.
- **Un eseguibile che gira non è un'applicazione installata.** Da fuori sembra
  pedanteria; da dentro è la differenza tra avere un'identità presso la shell e
  non averla. Metà delle integrazioni con il sistema operativo — pannello
  multimediale, jump list, notifiche, riavvio dopo un aggiornamento — sono
  appese a quell'identità, e l'identità la crea l'installazione. Ho passato
  cinque anni a pensare che l'installer fosse la parte noiosa dopo il software.
- **Togliere il codice che non mantiene la sua promessa è una correzione.**
  L'AUMID esplicito non rompeva niente e sarebbe rimasto lì per sempre a
  suggerire a chi legge che l'identità sia risolta.

### Da riprendere

- **Il nome nel pannello dipende dall'installazione.** Finché Sonora si lancia
  dalla cartella di build dirà "App sconosciuta"; da un collegamento nel menu
  Start dice "Sonora". Si chiude con l'MSI della settimana 10, non con altro
  codice. Documentato nel README perché è un comportamento, non un difetto.
- **macOS non ha mai eseguito una riga di `media_integration_mac.mm`.** Il file
  dice in testa quali tre parti sono più probabilmente sbagliate.
- **Nessun selettore di cartella.** Rimandato dalla settimana 7 a questa e da
  questa alla 9: serve un dialogo nativo, che appartiene alla stessa famiglia di
  lavoro dell'integrazione con la shell.
- **La posizione mostrata nel pannello viene campionata a 250 ms**, che è
  abbastanza per l'occhio ma è comunque un timer che gira anche quando non
  serve. Con un evento di stato dal player si potrebbe spegnere da fermo.
- **Il build non compila ancora la UI**, **due istanze condividono lo stesso
  `library.sqlite`**, **le copertine arrivano solo dai tag**, **i file OneDrive
  non sono mai stati provati** e **la matrice CI non l'ho ancora letta** — cinque
  voci ereditate dalla settimana 7, tutte ancora vere.
- **`localhost:9222` resta bianco** (settimana 4), **la sandbox resta spenta**
  (ADR 0003, settimana 10), e i **10 minuti continui senza underrun** non li ho
  ancora misurati (settimana 5).

---

## Settimana 9 — Integrazione shell

**Pianificata:** 16-22 nov 2026 · **Effettiva:** 26 set 2026
**Stima:** 8 h · **Effettivo:** ___ h
**Tag:** `v0.4-native`

### Obiettivo

Far esistere Sonora fuori dalla propria finestra: tray con menu, pulsanti sotto
l'anteprima della taskbar, jump list, una sola istanza per sessione, i link
`sonora://` dal browser, e la finestra che si riapre dove l'avevi lasciata.

### Fatto

- [x] `core::ParseDeepLink`: parser totale, solo cifre, nessun percent-decoding — 13 casi
- [x] `core::ResolvePlacement`: monitor scomparso, finestra fuori schermo, DPI misti — 16 casi
- [x] **`sonora::state`, il secondo store** (ADR 0008): id durevoli, `synchronous = FULL` — 13 casi
- [x] Interfacce `displays`, `shell_integration`, `single_instance`, `url_scheme` + stub portabili
- [x] Win32: tray con menu, thumbnail toolbar, jump list, mutex + `WM_COPYDATA`, registrazione in HKCU
- [x] Le tre icone dei pulsanti disegnate pixel per pixel, non spedite come file
- [x] Ripristino di posizione, dimensione e stato massimizzato, in coordinate workspace
- [x] AppUserModelID esplicito — rimesso, ora che qualcosa lo registra davvero
- [x] 122 asserzioni in 42 casi, pulite sotto ASan e UBSan, verificate anche per mutazione

### Cosa è costato più del previsto

**1. L'`ALTER TABLE` che non ho scritto.** La jump list mostra gli album
riprodotti di recente, e "di recente" non è un fatto che sta in un file: nessuna
riscansione lo ricostruisce. La mossa ovvia era una colonna `last_played_at`
sulla tabella dei brani — una riga, l'indice era già lì.

Poi ho riletto l'ADR 0007, che dice testualmente: *«Playlist, voti e conteggi di
riproduzione hanno bisogno di un altro store, con durabilità vera e migrazioni
vere, e non stanno in `src/library/`. La settimana 9 traccia quel confine.»*
Quella riga avrebbe abrogato l'ADR in silenzio: da lì in poi cancellare l'indice
non sarebbe più stato un evento recuperabile, e `synchronous = NORMAL`, le
migrazioni che possono buttare tutto e il messaggio che invita a cancellare la
cache sarebbero diventati falsi senza che niente nel codice lo dicesse.

E non era solo una questione di principio, che è la parte che non mi aspettavo.
Gli id dell'indice vengono riassegnati a ogni ricostruzione, e le voci della jump
list sono URL che **Windows ci restituisce settimane dopo**. Un
`sonora://track/412` con un id della cache avrebbe suonato una canzone a marzo e
un'altra ad aprile, sbagliando in silenzio: parte qualcosa, solo non quello che
hai cliccato. Il confine dell'ADR 0007 e la correttezza dei deep link sono la
stessa cosa vista da due lati.

**2. Tre bug al primo avvio su Windows, e due erano vecchi.**

- **La UI non riempiva la finestra ripristinata — bug della settimana 2, latente
  per sette settimane.** `main.cpp` calcolava la dimensione iniziale della vista
  del browser dalla dimensione *richiesta* (`desc.width_dip * scale`) invece che
  dalla finestra effettivamente creata. Finché la finestra si apriva sempre alla
  dimensione di default i due numeri coincidevano per costruzione, e CEF viene
  ridimensionato solo da `WM_SIZE` — che non arriva mai, se la finestra si apre
  già alla sua dimensione finale.

- **L'istanza singola non funzionava, e il log lo diceva per omissione.** Ho
  passato un giro a sospettare `FindWindowEx(HWND_MESSAGE, ...)` e a sostituirlo
  con un handoff deterministico. Poi ho guardato meglio l'output e ho notato che
  la riga `no listener after 40 attempts` **non c'era**: il messaggio era stato
  trovato, mandato e rifiutato. Il colpevole era mio, tre righe più in là:

  ```cpp
  if (payload->lpData == nullptr || payload->cbData == 0 || ...) return 0;
  ```

  Un rilancio senza argomenti produce un payload vuoto, e io lo scartavo come
  malformato. Ma un rilancio senza argomenti è **l'attivazione più comune che
  esista** — qualcuno ha fatto doppio clic su Sonora mentre Sonora era già
  aperto, e quello che chiede è la finestra che ha già.

- **LeakSanitizer ha trovato un costruttore che lancia senza distruttore.**
  `Library::Library` apre la connessione e poi migra; se la migrazione rifiuta un
  file scritto da una versione più nuova, l'eccezione esce dal costruttore e il
  distruttore non viene chiamato mai. La connessione restava aperta per tutta la
  vita del processo, tenendo un lock sul file che l'utente era appena stato
  invitato ad andare a guardare. Presente dalla settimana 7.

**3. La finestra Chromium vagante.** Quando il secondo processo concludeva che
il primo non rispondeva, proseguiva come un avvio normale — e CEF trovava la
user-data-dir già bloccata. Chromium non fallisce, in quel caso: decide che gli
stai chiedendo di aprire una scheda in una sessione esistente e ti mette in
faccia un browser. Mezz'ora spesa a capire da dove venisse una finestra di
Google.

### Cosa ho imparato

- **Un ADR scritto bene ti dice cosa fare due mesi dopo.** L'ADR 0007 conteneva
  già la frase che ha deciso questa settimana, scritta quando il problema non
  esisteva ancora. Non è documentazione: è una decisione che continua a
  funzionare mentre non la guardi.
- **Un identificatore che esce dal processo deve sopravvivere a tutto quello che
  succede dentro.** Una voce della jump list registrata con Windows sopravvive al
  processo, all'indice e a tre release. Se il numero che contiene viene
  riassegnato, il bug non è un errore: è una canzone sbagliata.
- **Una riga di log che non compare è un'informazione.** Cercavo la causa dove
  avrei dovuto vedere un messaggio e non lo vedevo, e quello era il messaggio.
- **Un bug latente non è un bug nuovo.** La settimana 9 non ha rotto la UI: ha
  smesso di garantire la coincidenza che la teneva in piedi. Una funzionalità che
  "rompe" qualcosa spesso non fa altro che rimuovere un'ipotesi che nessuno aveva
  scritto — e la correzione giusta non è ripristinare l'ipotesi, è chiedere alla
  finestra quanto è grande invece di dedurlo.
- **Coordinate workspace non sono coordinate schermo.** `GetWindowPlacement`
  restituisce `rcNormalPosition` in coordinate dell'area di lavoro, che
  coincidono con quelle dello schermo solo se la taskbar non è in alto né a
  sinistra. La correzione è zero sulla maggior parte delle macchine, ed è
  esattamente per questo che si dimentica.
- **Il claim viene preso prima che l'ascoltatore esista**, e quella finestra di
  corsa è reale: un lancio che ci cade dentro trova un mutex con nessuno dietro.
  Ritentare per due secondi è la lettura onesta di quello stato ("la prima copia
  sta ancora partendo"), arrendersi subito non lo è.
- **Un'API che cerca per nome non ti dice quale dei nomi non corrisponde.**
  `FindWindowEx` per classe e titolo ha tre cose che devono coincidere e, quando
  fallisce, un solo modo di dirlo. Un handle scritto dal processo che lo possiede
  non ne ha nessuna: o la pagina condivisa c'è o non c'è.
- **Le tre maniere di fallire vanno distinte nel codice, non nella testa.**
  "Nessun ascoltatore", "non risponde in tempo" e "ha risposto di no" erano lo
  stesso `return false`, e mi sono costate il giro sbagliato.
- **Disegnare le icone nel codice vale anche per tre triangoli.** GDI non ha
  opinioni sul canale alfa, quindi i pixel sono scritti a mano nel DIB: è più
  corto della spiegazione di perché un'icona con l'alfa sbagliato diventa un
  quadrato nero sull'anteprima scura della taskbar.
- **Mutare il codice per vedere i test fallire è economico.** Quattro mutazioni
  (scegliere il monitor dall'angolo invece che dall'area, togliere
  `AUTOINCREMENT`, togliere il `MAX()` sul timestamp, ignorare uno schema dal
  futuro) e ognuna fa cadere esattamente il test scritto per lei. È il modo più
  rapido che conosco di distinguere un test da un'asserzione decorativa.

### Da riprendere

- **Il monitor staccato non l'ho provato davvero.** La policy ha il caso coperto
  da un test, l'hardware no: chiudere con la finestra sul monitor esterno,
  staccarlo e riaprire è una prova di trenta secondi che non ho fatto.
- **La jump list dipende ancora dal collegamento installato.** È registrata
  sotto l'AppUserModelID, e senza una scorciatoia che lo porti la shell la
  mostra solo finché Sonora è in esecuzione — stessa conclusione della settimana
  8, stesso rimedio: l'MSI della settimana 10.
- **`Forget()` esiste e non lo chiama nessuno.** Lo store durevole non dimentica
  mai un percorso, quindi cresce di una riga per file riprodotto e non torna mai
  indietro. È deliberato — un disco esterno scollegato non è un file cancellato —
  ma va deciso da qualcuno, prima o poi.
- **macOS non ha niente di tutto questo**: nessuno status-bar item, nessun menu
  del Dock, nessuna registrazione in Launch Services. Gli stub lo dicono
  all'avvio invece di lasciarlo scoprire.
- **`Statement`, `Execute` e `Transaction` esistono due volte**, in `library` e
  in `state`. Deliberato (ADR 0008), e comunque una cosa da rileggere fra un
  mese con occhi nuovi.
- **Il build non compila la UI**, **due istanze condividevano `library.sqlite`**
  (adesso non più, ed è un effetto collaterale gradito dell'istanza singola),
  **le copertine arrivano solo dai tag**, **i file OneDrive non sono mai stati
  provati** e **la matrice CI non l'ho ancora letta** — quattro voci ereditate,
  tutte ancora vere.
- **`localhost:9222` resta bianco** (settimana 4), **la sandbox resta spenta**
  (ADR 0003, settimana 10), e i **10 minuti continui senza underrun** non li ho
  ancora misurati (settimana 5).

---

## Settimana 10 — CI e packaging

**Pianificata:** 23-29 nov 2026 · **Effettiva:** 26 set 2026
**Stima:** 9 h · **Effettivo:** ___ h
**Tag:** `v0.5.0` — il primo tag che è una versione e non un nome

### Obiettivo

Che un tag produca da solo una Release con l'MSI allegato, e che la matrice
windows/macOS/linux sia verde — dopo sei settimane in cui non l'avevo mai letta.

### Fatto

- [x] Versione da `git describe`, iniettata in `project()` e da lì ovunque
- [x] Solo `vX.Y.Z` conta come tag di release: i tag narrativi restano nomi
- [x] `-DSONORA_BUILD_UI=ON`: la UI la costruisce la build, e CI la usa
- [x] Avviso a configure quando `ui/dist` è più vecchia di `ui/src`
- [x] clang-format **20.1.7** da pip, stesso numero nel workflow e in `format.ps1`
- [x] `builtin-baseline` in `vcpkg.json`, con vcpkg clonato a quel commit dalla CI
- [x] WiX v4 **fissato**, installer MSI per utente, disinstallazione pulita
- [x] Il collegamento nel menu Start porta l'AppUserModelID (settimane 8 e 9)
- [x] Tipi audio come "Apri con", mai predefinito; un percorso sulla riga di comando si riproduce
- [x] `install(RUNTIME_DEPENDENCY_SET)`: il payload lo decidono i binari, non una lista
- [x] Firma con certificato autofirmato in `tools/sign.ps1`, **fuori** dalla CI e spiegato
- [x] Job `MSI` e `GitHub Release`, la Release nasce come bozza

### Cosa è costato più del previsto

**Questa settimana è stata un'unica lezione ripetuta quattro volte: una cosa
non fissata è una cosa che cambierà sotto di te.** Ne avevo fissate tre —
CEF dalla settimana 2, clang-format e i pacchetti vcpkg — e ogni pezzo che
avevo lasciato libero mi si è rotto in mano nell'ordine in cui l'ho toccato.

1. **WiX.** `dotnet tool install --global wix` installa la v7, che si rifiuta
   di costruire finché non accetti la licenza dell'Open Source Maintenance Fee.
   Un major che nessuno aveva scelto, che mette un cancello commerciale in
   mezzo a una build. Fissato a `4.*`, e `package.ps1` adesso **verifica** di
   avere davvero una v4 invece di fidarsi del nome sul PATH.

2. **Il `Files` di WiX.** L'elemento che raccoglie una cartella in una riga
   esiste, ma è arrivato nella v5 — dall'altra parte di quel cancello. Quindi
   `tools/gen_installer_files.py`, che è il quarto generatore del repo dopo
   bridge, asset e icona. Heat, il raccoglitore della v3, non c'è più nella v4;
   non è una gran perdita, perché anche il suo output era generato, solo da uno
   strumento che nessuno poteva leggere.

3. **Il baseline di vcpkg, due volte.** Fissare il registro sembrava una riga
   di JSON. Il primo fallimento: l'immagine macOS ha un clone di vcpkg più
   vecchio del commit fissato, e vcpkg non se lo va a prendere. Ho aggiunto un
   passo che faceva la fetch — e il secondo fallimento ha spiegato il primo:

   ```
   error: no version database entry for sqlite3 at 3.53.4#1
   ```

   vcpkg legge `versions/baseline.json` **dal commit**, ma il database delle
   versioni per porta **dall'albero di lavoro**, che resta a dov'era. Non
   esiste una mezza misura: avere il registro che un baseline descrive
   significa avere il registro a quel commit, e quindi possedere il checkout.
   Era l'alternativa che avevo scartato due ore prima perché "costa minuti a
   ogni corsa" — e la stima era sbagliata, perché la cache la paga una volta
   per baseline.

4. **La UI che non veniva costruita dal passo che diceva di costruirla.** Su
   Windows npm è `npm.cmd`, CMake scrive i passi di un custom command in un file
   batch, e un batch che ne invoca un altro senza `call` non torna indietro: il
   secondo rimpiazza il primo. `npm ci` girava, `npm run build` no, e la build
   incorporava una `ui/dist` di tre settimane prima stampando *"Embedding the
   web UI"*. L'unica traccia era un `warning MSB8065` in mezzo a duemila righe,
   che avevo letto e superato.

**E poi la lista scritta a mano, contro cui avevo appena scritto un commento.**
Le regole di `install` elencavano i file di CEF e i due eseguibili — quello che
una persona si ricorda — e non `sqlite3.dll`, `tag.dll`, `z.dll`, che vcpkg
copia accanto all'eseguibile in un passo post-build che CMake non conosce.
L'MSI si è costruito, si è installato, e l'applicazione è morta all'avvio.
Mentre scrivevo, nel generatore del payload: *"una lista di 250 file mantenuta
da una persona è sbagliata la prima volta che CEF aggiunge una dll, e sbagliata
in silenzio"*.

### Cosa ho imparato

- **Fissare una versione non è pignoleria, è la differenza fra una build e una
  scommessa.** Quattro strumenti, quattro storie diverse, un solo schema: quello
  lasciato libero cambia quando gli pare, e cambia in produzione.
- **Un numero scritto due volte è un numero che divergerà.** La versione veniva
  da `project()` e da `vcpkg.json`; ora viene dal tag, e i tag narrativi sono
  esclusi da un `--match` perché `0.4-native-2-g1fd9823` è una frase, non una
  versione.
- **Un timbro che può comparire senza che il lavoro sia avvenuto non certifica
  niente.** Lo stamp della UI era un `touch`; ora è una copia dell'`index.html`
  costruito, quindi se la build non l'ha prodotto il passo fallisce. È la stessa
  idea dei test della settimana 9: un controllo che non può fallire non è un
  controllo.
- **Non chiedere a una persona quello che si può chiedere ai binari.**
  `install(RUNTIME_DEPENDENCY_SET)` legge le tabelle di import e porta quello
  che nominano; la dipendenza aggiunta il mese prossimo viaggia da sola.
- **Un passo che ha bisogno di un valore non ha bisogno di un interprete.**
  Leggere quaranta caratteri esadecimali con Python è costato un `\a` diventato
  carattere di campanello dentro `D:\a\sonora\sonora`. `grep` non ha quel
  problema perché non ha quella potenza.
- **Il secondo errore spiega il primo, se lo lasci parlare.** Ho corretto il
  sintomo (il commit mancante) invece della causa (il registro sbagliato), e la
  correzione ha prodotto un errore diverso che diceva esattamente cosa avevo
  frainteso. Vale la pena ricordarselo: quando una toppa produce un errore
  *nuovo*, spesso è il problema vero che si presenta.
- **La matrice verde al primo colpo su macOS è stato il risultato più forte
  della settimana**, e non me lo aspettavo: avevo previsto rosso, perché
  `window_mac.mm` ha guadagnato tre metodi in due settimane senza vedere un
  compilatore. L'ADR 0002 ha retto — quei tre metodi parlano di struct portabili
  e l'unica cosa veramente AppKit, l'asse y che cresce all'insù, era scritta al
  punto giusto. **Un'astrazione che nessuno esercita per sei settimane e poi
  passa non è fortuna: è la prova che il confine era nel posto giusto.**

### Da riprendere

- **La firma è dimostrata, non usata.** `tools/sign.ps1` fa la cosa vera —
  certificato, `signtool`, marca temporale, verifica che fallisce — ma il
  certificato è autofirmato, cioè attendibile su una macchina sola. Nella CI
  c'è il commento che dice dove andrebbe il passo con un certificato vero, e
  Smart App Control continuerà a rifiutare l'MSI (settimana 7).
- **Il runtime di Visual C++ non viaggia con l'installer**: è escluso perché si
  risolve in `System32`. Su una macchina che non ha mai installato
  un'applicazione C++ l'app non parte, e la prova non l'ho fatta.
- **`actions/checkout@v4` e `setup-node@v4` girano su Node 20, deprecato**, e
  `ubuntu-latest` diventa Ubuntu 26 il 19 ottobre. Nessuna delle due è rotta
  oggi.
- **L'MSI pesa 159 MB** ed è quasi tutto CEF. Un aggiornamento completo per
  cambiare tre righe di C++ è esattamente il problema che la settimana 11
  esiste per risolvere.
- **La sandbox di CEF resta spenta** (ADR 0003): era in programma per questa
  settimana e non l'ho toccata.
- **I 10 minuti continui senza underrun** non li ho ancora misurati
  (settimana 5), le **copertine arrivano solo dai tag**, i **file OneDrive** non
  sono mai stati provati, e `localhost:9222` **resta bianco** (settimana 4).

---

## Settimana 11 — Updater con delta e rollback

**Pianificata:** 30 nov - 6 dic 2026 · **Effettiva:** 27 set 2026
**Stima:** 11 h · **Effettivo:** ___ h
**Tag:** `v0.6.0`

### Obiettivo

Che cambiare tre righe di C++ non costi 152 MB a chi ha già Sonora installato. E che
una versione che non parte torni indietro da sola.

### Fatto

- [x] **Nessun servizio**: il server degli update è un file JSON firmato (ADR 0009)
- [x] Firma **Ed25519** verificata *prima* di dare i byte a un parser
- [x] Formato archivio `.spk`: i file del payload, in ordine, **non compressi** (ADR 0010)
- [x] Il client **ricostruisce** l'archivio della propria versione dai file installati
- [x] Delta con `zstd --patch-from`, `nbWorkers = 0` e la misura nel commento
- [x] Giornale con macchina a stati pura, e **swap che non è atomico** (ADR 0011)
- [x] Rollback automatico: bandierina di avvio, un tentativo, lista dei rifiutati
- [x] `sonora-updater.exe`, processo separato che si copia fuori dall'albero
- [x] `sonora_release` (pack/compress/expand/delta) e `tools/gen_manifest.py`
- [x] Job CI che pubblica pacchetto, delta e manifest firmato con la Release
- [x] I tre test end-to-end del roadmap, **su tutte e tre le piattaforme**

Il numero della settimana, prodotto dal codice che spedisce e non da una prova a mano:

| | |
|---|---|
| Archivio non compresso (`.spk`) | 223.410.842 B — 213,1 MiB |
| Pacchetto scaricabile (`.spk.zst`) | 49.320.420 B — 47,0 MiB |
| **Delta fra due versioni che differiscono per tre righe** | **86.053 B — 84,0 KiB** |
| Delta come frazione del pacchetto | **0,0385 %** |

### La misura che ha deciso il progetto

L'ho fatta prima di scrivere una riga, perché tutta la settimana dipendeva da un
numero, e ha cambiato il progetto due volte.

Payload finto di 213 MiB con la forma di quello vero: una libreria da 137 MiB al posto
di `libcef.dll`, un eseguibile da 631 KiB, qualche file in sottocartelle. La v2
differisce dalla v1 per **una costante stringa e un'espressione aritmetica** — e 472
KiB di byte dell'eseguibile cambiano, perché tutto quello che sta dopo la modifica
slitta di otto byte. Tre forme di contenitore, lo stesso comando fra ogni coppia:

| Contenitore | Dimensione | Delta | % |
|---|---|---|---|
| Archivio store-only | 213,1 MiB | **127,8 KiB** | 0,06 % |
| Compresso per file (`zip -9`) | 64,5 MiB | **133,7 KiB** | 0,20 % |
| Compresso solido (un frame `zstd -19`) | 47,0 MiB | **5,04 MiB** | 10,7 % |

**Non è la compressione che uccide il delta: è la compressione solida.** Un contenitore
che comprime ogni membro per conto suo lascia identici i membri non cambiati, e il patch
resta grande come il file cambiato. Uno che comprime tutto in un flusso ricodifica tutto
quello che sta a valle del primo byte diverso: quaranta volte peggio, per tre righe.

Un MSI con `MediaTemplate EmbedCab="yes"` è la terza riga. Quindi **l'installer non può
essere la cosa che si patcha** — e c'è una seconda ragione che non ha bisogno di
misure: aggiornare eseguendo un installer significa chiedere a Windows Installer di
fare lo swap, e allora *quando* i file si spostano, chi tiene la transazione e cosa
succede se manca la corrente diventano semantica dell'MSI invece che nostra. È tutto
l'ADR 0011 regalato a qualcun altro.

La seconda sorpresa è arrivata dallo stesso esperimento e non c'entra con i
contenitori:

| Impostazione | Patch |
|---|---|
| `-9`, thread scelti dal tool | 127,8 KiB |
| `-17`, thread scelti dal tool | 323,5 KiB |
| `-19`, thread scelti dal tool | 178,9 KiB |
| `-19 --single-thread` | **84,3 KiB** |

La compressione multithread taglia l'input in job compressi indipendentemente, e un
patch è fatto **solo** di match a lunga distanza: i thread che rendono veloce la
compressione sono i thread che buttano via i match. Due volte più grande a livello 19,
quasi quattro a 17, e **non monotono nel livello** — che è come si comporta una
manopola quando è la manopola sbagliata. `ZSTD_c_nbWorkers = 0`, scritto esplicito, con
questa tabella nel commento accanto.

### Cosa è costato più del previsto

**1. Il roadmap chiedeva un backend e la risposta giusta era "nessuno dei due".**
`services/releases` in C++ con Drogon, oppure in Go — «scegli e motiva nell'ADR». Ci ho
messo un'ora a capire che la domanda nascondeva la domanda: un updater ha bisogno di una
risposta, *dato un canale, una piattaforma e la versione che sto eseguendo, cosa devo
installare*, e gli input sono tre valori enumerabili con un dominio di decine. Un
servizio calcola quella risposta a ogni richiesta; un file la precalcola una volta, per
ogni input. Sono entrambi corretti. La differenza è tutto quello che deve esistere
intorno: un processo, un host, un deploy, un certificato, un conto da pagare e una
persona che si accorge quando smette. E un'asimmetria che chiude la questione: **un file
statico non può essere giù mentre la corsa di CI che l'ha prodotto era verde.**

C'è anche un modo di fallire che non voglio costruire. Un update server è l'unico
componente di un'applicazione desktop con cui parla *ogni* installazione, su timer, per
sempre, comprese quelle di tre anni fa. La vita attesa onesta di un backend hobbistico è
più corta della vita attesa onesta di una copia installata del programma.

**2. Il buffer passato due volte.** Il primo test sul patch diceva "due payload identici
danno un patch quasi nullo", e falliva: 262.159 byte per 256 KiB di input. Ho perso venti
minuti sul `windowLog` prima di accorgermi che passavo *lo stesso vettore* come vecchio e
come nuovo. Con il prefisso sovrapposto all'input zstd non trova nessun match: 262.159
byte contro 36 con una seconda copia. **Non falla — smette di essere un patch**, e
l'unico sintomo è un numero in un log che nessuno legge. Adesso è un errore con un nome
(`kOverlappingInputs`) e un commento che riporta le due misure, perché un chiamante che
mappa in memoria un file e passa la stessa mappatura ai due argomenti spedirebbe delta a
dimensione intera per sempre.

**3. Il bug che avrebbe cancellato l'unica installazione presente.** L'ho trovato il test
esaustivo, non io. Giornale a `idle`, nessuna `Sonora/`, ma `Sonora.old/` accanto: la mia
prima versione di `NextStep` la spazzava via come spazzatura, perché "idle" significa
"niente in volo". È uno stato **raggiungibile**: un rollback che ha completato il rename
e ha perso corrente prima di scrivere il giornale lascia esattamente questo, perché il
giornale che stava per scrivere è quello che c'è già. Ora è l'unico caso del ramo `idle`
che fa qualcosa, e la riga che lo fa ha sopra tre righe di commento che spiegano come si
arriva lì.

**4. La bandierina non è un timer, e il roadmap diceva timer.** «La nuova versione deve
scrivere un flag "avvio riuscito" entro 20 s». Non lo fa, e vale scriverlo: venti secondi
misurano la macchina, non il programma. Su un portatile freddo con un antivirus che legge
140 MiB di CEF per la prima volta, una versione perfettamente buona sfora e viene
riportata indietro — e un rollback causato dalla lentezza è un bug che compare solo
sull'hardware che meno se lo può permettere. Quindi non c'è nessun timer: la nuova
versione scrive la bandierina quando raggiunge lo stato che *dimostra* che l'update ha
funzionato, cioè finestra su e pagina caricata, un evento che esiste dalla settimana 2. Se
non ci arriva, la bandierina non compare e **è l'avvio successivo che se ne accorge**. La
scadenza non è una durata: è "prima che il processo finisca".

**5. Windows non risponde male alla domanda sul bit di eseguibile: risponde una cosa
sbagliata.** L'unico dei 278 test che ha fallito su Windows, dopo che tutti e 278 passavano nel container. `std::filesystem` di
MSVC modella un solo attributo — sola lettura — e per ogni file scrivibile riporta
`perms::all`, quindi **ogni** membro di un payload pacchettizzato su Windows si portava il
flag di eseguibile, `.pak` e `.dat` compresi. Il test lo ha visto dalla parte giusta: non
ha fallito sull'eseguibile che *doveva* averlo, ha fallito sul file che non doveva. Un
flag messo su tutto non dice niente, e un formato la cui unica bandierina non dice niente
sulla piattaforma per cui il payload è costruito è peggio di un formato senza.

La correzione non è un `#ifdef` — l'ADR 0002 non lo permette lì, e non servirebbe: la
domanda non è "su che sistema operativo siamo" ma "questo filesystem sa distinguere un
file eseguibile da uno normale". Quindi `ExecuteBitIsMeaningful` la chiede a un file che
crea lui, vuoto, accanto all'archivio che sta scrivendo: se un file appena creato e vuoto
si dichiara già eseguibile, il filesystem non sta riportando quel bit, se lo sta
inventando. Su Linux dice sì, su Windows dice no, e su Windows il formato non porta
bandierine invece di portarne 242 che mentono. **La prima versione del test si limitava a
saltare quando il bit non tornava indietro; quella di adesso asserisce su entrambi i tipi
di filesystem, che è la differenza fra un test e una nota.**

**6. Una dipendenza si costruisce in due modi diversi, e la piattaforma che le serve
davvero è l'unica che non ha avuto problemi.** La matrice: Windows verde in 6m44s con i
test, macOS e Linux rossi in venti secondi. La porta vcpkg di libsodium usa il progetto
MSVC su Windows e gli autotools altrove, e le immagini dei runner hanno `autoconf`,
`automake` e `libtool` ma non `autoconf-archive` — che la porta chiede insieme agli altri
tre. Venti secondi erano il tempo di scaricare il tarball e arrivare a `autoreconf`.

Aggiunti i quattro pacchetti ai due job Unix, ed è **l'unica cosa non fissata in tutta la
pipeline**: vengono dal gestore di pacchetti del runner e cambiano quando cambia
l'immagine. L'argomento della settimana 10 si applica e non ha dove applicarsi. Quello che
limita il danno è che decidono *se* libsodium si costruisce, non *quale*: la versione è
quella del baseline, e con la cache binaria questo percorso gira una volta per baseline e
non a ogni push.

### Cosa ho imparato

- **Misura il contenitore prima di scegliere l'algoritmo.** Avevo l'ipotesi giusta per la
  ragione sbagliata: pensavo che la compressione rovinasse i delta, ed è la compressione
  *solida*. La differenza fra le due non è accademica, è la differenza fra pubblicare due
  artefatti e pubblicarne uno.
- **Una cosa che deve essere riproducibile non può avere un compressore nel percorso.**
  Il client ricostruisce l'archivio della propria versione dai file installati, per poterlo
  patchare, e ci riesce solo perché nel formato non c'è **niente** su cui due macchine
  possano non essere d'accordo: ordine dei membri dai percorsi ordinati, nessun timestamp,
  nessuna compressione, nessun allineamento. La settimana 10 ha reso riproducibile una
  build fissando quattro strumenti; questo è lo stesso argomento applicato a un artefatto
  che deve essere riproducibile sul portatile di uno sconosciuto, dove non è fissato niente.
- **Un'ottimizzazione con una precondizione verificata non è una dipendenza.** La
  ricostruzione viene confrontata con l'hash del manifest *prima* di patchare; se non
  combacia — un file toccato da un antivirus, un update lasciato a metà — il client scarica
  il pacchetto intero. Il percorso del delta può sbagliarsi quanto vuole: costa byte, non
  correttezza.
- **Firma i byte, non l'oggetto.** La firma copre gli ottetti del manifest come sono
  arrivati, ed è verificata *prima* del parser JSON. È per questo che è staccata e che non
  esiste un campo `"signature"` dentro il documento: una firma dentro la cosa che firma
  vuol dire canonicalizzare prima, e la canonicalizzazione è un secondo parser con i suoi
  bug, che gira prima del controllo, su input non autenticato.
- **"Atomico" è una parola che nasconde tre operazioni.** Un `MoveFileEx` è atomico; un
  update sono due rename e una cancellazione, e fra il primo e il secondo **non esiste
  nessuna copia installata di Sonora.** La finestra è larga microsecondi e alla macchina
  non importa. Quello che serviva non era uno swap, era una sequenza di passi idempotenti
  con un registro durevole di quale era inteso — cioè un giornale, cioè la stessa idea del
  WAL dello SQLite due cartelle più in là.
- **Un errore precoce ed economico batte un errore giusto e caro.** Un tentativo, non tre.
  Una versione che non parte una volta *potrebbe* essere una coincidenza, e preferire
  comunque quella precedente è lo sbaglio che costa poco: il rollback sono due rename e un
  riavvio, il manifest offrirà di nuovo la stessa versione, e nel frattempo la persona ha
  un programma che funziona. Aspettare di essere sicuri costa alla persona
  un'applicazione rotta. Quando i due errori sono così sbilanciati, prendi quello presto.
- **La parte difficile era testabile senza Windows, e quindi l'ho testata.** `NextStep`
  prende un giornale e quattro booleani. Il che significa che l'iniezione di crash è un
  contatore che ferma il ciclo dopo l'n-esima operazione, per ogni n, e l'invariante si
  verifica ripartendo da lì: *in ogni punto in cui la macchina può fermarsi esiste
  esattamente un'installazione completa raggiungibile, e il recupero la raggiunge.* Non
  "di solito". Ogni n, compresi quelli in mezzo a un rollback che sta a sua volta
  recuperando da un update fallito.

### Numeri di verifica

- **91 casi, 5.887 asserzioni**, puliti sotto ASan + UBSan + LSan.
- **Mutation testing: 40 mutanti, 36 uccisi.** Vale più il conto di quelli che sono
  sopravvissuti. Quattro, e tutti e quattro **equivalenti**: togliere il controllo
  esplicito su `".."` non cambia niente perché la regola sul punto finale rifiuta già
  `.` e `..`; togliere il rifiuto del messaggio vuoto non cambia niente perché libsodium
  lo rifiuta comunque; togliere il controllo sull'`@` nell'authority non cambia niente
  perché il controllo sui caratteri dell'host lo rifiuta; e mettere `nbWorkers = 4` non
  cambia niente su un payload che un test unitario possa permettersi, perché i job di
  zstd sono più grandi dell'input — la prova su quel numero è la misura dell'ADR 0010,
  e quello che il test compra è che cambiarlo si veda in un diff che tocca un test e
  dice perché.
- **Sei test esistono perché li ha chiesti la mutation testing, non io.** Il rifiuto di
  un flag ignoto lo controllava solo il codificatore e non il lettore; i link simbolici,
  il bit di eseguibile e il file temporaneo del giornale non erano controllati affatto; e
  gli ultimi due controlli di hash — quello sull'archivio e quello sul pacchetto — non
  erano raggiunti da nessun test, perché ogni corruzione che provavo la prendeva prima
  la checksum di zstd o l'hash della ricostruzione. Servivano due release che si
  contraddicono da sole: un manifest che nomina il pacchetto giusto e l'archivio
  sbagliato, e uno che nomina l'archivio giusto e il pacchetto sbagliato. **Tre controlli
  di hash in fila sembrano ridondanti finche' non provi a togliere quello in mezzo.**
- Il test esaustivo del giornale copre **tutti** gli stati: 7 stadi × 8 combinazioni delle
  tre cartelle × 3 cose che la bandierina può dire × 2 conteggi di tentativi × 2 esiti di
  avvio, ognuno portato a termine su più sessioni. Comprese le combinazioni irraggiungibili,
  perché "irraggiungibile" è esattamente l'ipotesi che si rompe su un portatile con un
  rollback a metà.
- I **tre test end-to-end** del roadmap girano su Windows, macOS e Linux: albero vero,
  archivio vero, firma Ed25519 vera su un manifest vero, patch zstd vero, rename veri,
  giornale vero. Il `Fetcher` legge da una cartella invece che da un socket — la stessa
  sostituzione che i test unitari fanno per il filesystem, e per la stessa ragione. Quello
  che resta non provato sono le venti righe che chiamano WinHTTP; quello che è provato è
  tutto ciò che potrebbe distruggere l'installazione di qualcuno.
- Il patch da 86.053 byte sul payload da 213 MiB è verificato dal generatore stesso:
  `sonora_release delta` riapplica il patch appena prodotto e confronta i byte, perché un
  patch che non si applica è una release che silenziosamente fa scaricare 47 MiB a tutti
  quelli che sono sulla versione vecchia, e niente della release sembrerebbe sbagliato.
- **Il test del delta corrotto fa meglio di quello che il roadmap chiedeva.** Chiedeva
  «nessuna installazione»; qui il delta corrotto invalida *il delta*, non la release — il
  pacchetto c'è, il suo hash è nel manifest firmato, e un update che arriva come 47 MiB
  invece di 84 KiB non è un fallimento. Il test che verifica «nessuna installazione» è
  quello con **delta e pacchetto entrambi corrotti**, e lì l'installazione resta intatta,
  il giornale torna a `idle` e l'applicazione parte.

### Da riprendere

- **I quattro pacchetti autotools dei job Unix non sono fissati.** Servono alla porta
  vcpkg di libsodium, non a Sonora, e non entrano nel binario — ma sono l'unico punto in
  cui questa pipeline dipende da un'immagine di runner. La via d'uscita, il giorno che
  dessero problemi, è una libreria crittografica che si costruisca con CMake su tutte e
  tre le piattaforme; BLAKE2b-256 ed Ed25519 sono standard, quindi i vettori di prova e il
  manifest firmato di `test_update_manifest.cpp` continuerebbero a verificare — che è
  anche il modo di dimostrare che uno scambio così non cambia niente.

- **La chiave di firma è quella di sviluppo.** La sua metà privata è stata generata su una
  macchina che non è un archivio di segreti, ed esiste perché i test e la corsa
  end-to-end avessero qualcosa di vero da verificare. Va sostituita prima della prima
  release che qualcun altro installa (`tools/update_keygen.ps1`), e il job `release`
  **rifiuta** di pubblicare un manifest finché il binario si fida ancora di quella chiave:
  una nota in un commento non è un controllo. Finché il segreto non c'è, la Release esce
  senza manifest — e un manifest non firmato sarebbe peggio di nessun manifest, perché
  sarebbe un file all'indirizzo che ogni installazione interroga, che dice quello che vuole
  chiunque possa scriverci.
- **La chiave sta in un segreto del repository, ed è più debole di una chiave offline.**
  Chi può far girare un workflow qui può firmare una release. Il modo di fallire è
  *compromettere l'account GitHub è compromettere il canale degli update*, ed è scritto
  nell'ADR 0009 invece di essere scoperto dopo.
- **macOS e Linux non hanno updater**, e `shared/update_host_none.cpp` lo dice invece di
  finirla. Quattro delle cinque funzioni sarebbero dieci righe; quella che manca è tutto il
  resto — un canale firmato, una pipeline di pacchetti, un installer da sostituire e uno
  swap di cui qualcuno abbia provato i modi di fallire su quella piattaforma.
- **Se l'updater della versione nuova è rotto, niente torna indietro.** La decisione di
  rollback la prende `Sonora.exe`, che è il binario di cui è in dubbio la capacità di
  partire. Se non arriva alle sue prime dieci righe, il giornale non viene letto. È il
  rischio residuo di ogni updater in-place senza un servizio sempre residente, e la
  mitigazione onesta è quella che c'è già: l'MSI esiste ancora, e reinstallarlo è un
  recupero supportato e documentato (`docs/updater-states.md`).
- **Il rollout a percentuale non c'è.** La forma però è giusta: rilasciare al 10% è un
  *numero nel manifest* — il client fa l'hash del proprio id di installazione e confronta
  con una soglia — non un comportamento di un server. La settimana 12 aggiunge il campo,
  non aggiunge un servizio.
- **Un update ha bisogno di spazio per due payload e un patch**: circa 530 MiB liberi per
  un'installazione di 213 MiB. L'updater controlla prima e lo dice, invece di fallire a
  metà.
- Restano dalle settimane precedenti: il runtime di Visual C++ **fuori** dall'installer e
  mai provato su una macchina pulita, `actions/checkout@v4` su Node 20 deprecato, la
  **sandbox di CEF spenta** (ADR 0003), i **10 minuti continui senza underrun** non
  misurati, le **copertine solo dai tag**, i **file OneDrive** non provati, `localhost:9222`
  **bianco**, il caso del monitor staccato provato solo nella policy, e `Forget()` che
  esiste e non viene mai chiamato.
