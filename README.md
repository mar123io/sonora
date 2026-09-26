# Sonora

A native C++ desktop shell that hosts a web UI in CEF — with its own audio
engine, operating-system media integration, delta updates and a signed release
pipeline.

Sonora is built the way large desktop applications actually are: a native core
that owns audio and platform integration, a web layer that owns the interface,
and a versioned bridge between them so the two can ship independently.

> **Status: week 8 of 13.** The shell hosts a Chromium view, serves the UI over
> a custom `sonora://` scheme, and the two talk over a typed, versioned bridge
> generated from one schema. It plays music — a searchable library, a queue, and
> a gapless join between two tracks performed inside the device callback — and
> the operating system knows about it: Windows' media panel shows the track and
> its cover, and the keyboard's media keys work with the window minimised. See
> [ROADMAP.md](ROADMAP.md) for what lands when.

---

## What is built

| Area | State |
|---|---|
| Native window (Win32, per-monitor v2 DPI) | working |
| CEF embedded, separate helper process, external message pump | working |
| `sonora://app` custom scheme, embedded UI bundle | working |
| DevTools on F12, debug builds only | working |
| Typed native↔web bridge, generated from `schema/bridge.schema.json` | working |
| Capability negotiation, `SONORA_DISABLE_CAPS`, degraded UI | working |
| Push events, coalesced to 4 Hz, generated and typed on both sides | working |
| Audio engine: decoder → lock-free SPSC ring → device callback | working |
| `--play <file...>` for wav, flac and mp3, with an underrun counter | working |
| Queue, state machine, seek, volume ramp, **gapless** track change | working |
| `player` capability on the bridge, transport in the UI | working |
| SQLite + FTS5 library index, incremental scan, cover art | working |
| `library` capability, tracks addressed **by id — no path crosses the bridge** | working |
| Library UI: sidebar, virtualized track list, search, queue | working |
| Windows media panel (SMTC): title, artist, cover, transport buttons | working |
| Media keys with the window minimised or in the background | working |
| Icon and version resource, generated from the project's one version number | working |
| Playback state machine, media-session policy, asset store, bridge protocol, capabilities, coalescer, ring buffer, engine — unit tested | working |
| Platform abstraction, macOS backend (window + media, `MPNowPlayingInfoCenter`) | written, compiled in CI, **not tested on hardware** |
| Platform abstraction, Linux backend | stub; fails with a clear message at runtime |
| The media panel's *application name* | needs an installed shortcut — see below |

| Tray icon, jump list, single instance, protocol handler | week 9 |
| MSI packaging, CI matrix | week 10 |
| Delta updater with signature + rollback | week 11 |
| Staged rollout, crash reporting, perf gates | week 12 |

Performance numbers go here in week 12, together with the script that
reproduces them. Until then this table is the honest version.

**Windows 11 and Smart App Control:** a locally built Sonora is not signed, and
Smart App Control refuses unsigned binaries outright — the application does not
start and the message names a policy rather than a fault. There is no developer
exemption: Microsoft's own guidance for testing with it enabled requires it to be
in evaluation mode or off. Signed releases are week 13; until then a machine with
Smart App Control on cannot run a build of this repository.

**Graphics workarounds:** `SONORA_CEF_SWITCHES` passes extra Chromium switches,
space separated and without their leading dashes, and Sonora prints which ones it
applied. It exists because Chromium's DirectComposition presenter fails on some
AMD drivers — the GPU process dies and the window stays blank — and a project
pinned to one CEF build does not receive Chromium's driver blocklist updates:

```powershell
$env:SONORA_CEF_SWITCHES = "disable-direct-composition"
```

`SONORA_TRACE_BRIDGE=1` logs every bridge call before and after it runs; calls
that hold the UI thread for more than 250 ms say so on their own.

**Why the media panel says "Unknown app":** it is not a missing feature, and it
is worth knowing before you go looking for one. Windows takes the *name and icon
above* the media panel from a shortcut registered in the Start Menu, not from
the running executable — an application exists, as far as the shell is
concerned, once something installed it. A version resource, an icon resource,
an explicit AppUserModelID and the window's relaunch properties all change other
parts of the shell (the title bar, Alt-Tab, Explorer, Task Manager) and none of
them change that line. Sonora sets the relaunch properties anyway, because they
are the half the process itself can do, and prints whether they took:

```
shell identity: store=0x00000000 name=Sonora commit=0x00000000
```

Launched from the build folder, the panel says "Unknown app". Launched from a
Start Menu shortcut pointing at the same executable, it says **Sonora**, with
the icon. The MSI in week 10 closes it; until then the shortcut is the
workaround:

```powershell
$shell = New-Object -ComObject WScript.Shell
$link  = $shell.CreateShortcut("$env:APPDATA\Microsoft\Windows\Start Menu\Programs\Sonora.lnk")
$link.TargetPath = "C:\Workspace\Sonora\build\win-debug\bin\Debug\Sonora.exe"
$link.Save()
```

**Known gap:** the CEF sandbox is currently disabled, because `cef_sandbox.lib`
is published only for the static CRT while everything else in the build uses the
dynamic one. The reasoning and the plan to fix it are in
[ADR 0003](docs/adr/0003-serving-the-ui-over-a-custom-scheme.md).

---

## Building

### Prerequisites

| | |
|---|---|
| **MSVC** | Visual Studio 2022 or newer, or Build Tools, with the **Desktop development with C++** workload. |
| **CMake** | 3.28 or newer. |
| **vcpkg** | Any recent checkout, with `VCPKG_ROOT` set (persistently — a new terminal does not inherit `$env:`). |
| **Node** | 22 or newer, for the web UI. |
| **Python** | 3.10 or newer, for the asset and CEF tooling. |

### First build

```powershell
# 1. Pin CEF. Downloads ~1 GB once, verifies it, and writes cmake/cef_version.cmake.
python tools/pin_cef.py --platforms windows64 macosarm64

# 2. Build the web UI. The result is embedded into the executable.
cd ui; npm ci; npm run build; cd ..

# 3. Build the shell.
cmake --preset win-debug
cmake --build --preset win-debug
ctest --preset win-debug
```

`build\win-debug\bin\Debug\Sonora.exe` opens a window showing a page served from
`sonora://app/index.html`, with its origin diagnostics. `F12` opens DevTools in
debug builds only.

Commit the regenerated `cmake/cef_version.cmake`: the build never resolves
"latest" at configure time, so a checkout from a year from now builds the same
thing.

### Changing the bridge

`schema/bridge.schema.json` is the only place a method is defined. Add one,
rebuild, and the C++ will not compile until it is implemented — the handler
interface is generated pure virtual on purpose. The TypeScript gets the new
signature in the same build.

```powershell
cmake --build --preset win-debug   # regenerates the C++ half
cd ui; npm run generate            # regenerates the TypeScript half
```

The generated TypeScript is not committed: it is a build product, and `npm run
build` and `npm run typecheck` regenerate it first. See
[ADR 0004](docs/adr/0004-the-bridge-is-generated-from-a-schema.md).

From the DevTools console (or Chrome at the remote debugging URL):

```js
await sonora.shell.getVersion()
await sonora.shell.echo({ message: 'hi', repeat: 3 })
await sonora.shell.getCapabilities()
await sonora.diagnostics.getMetrics()
```

### Watching the degraded path

A capability can be switched off for one run, so the branch the UI takes when
something is missing is exercised on a current build rather than only against an
old one:

```powershell
./tools/run-dev.ps1 -DisableCaps diagnostics
```

The diagnostics rows turn amber and say they are switched off, the heartbeat is
never subscribed to, and `diagnostics.getMetrics` answers with error code 4
(`unavailable`) rather than 2 (`unknown method`) — the difference the page
branches on. `shell` is marked required in the schema and refuses to be
disabled: with `getCapabilities` gone there is nothing left to negotiate with.

### Playing a file

```powershell
.\build\win-debug\bin\Debug\Sonora.exe --play "C:\Music\track.flac"
```

No window, no Chromium, no bridge: a different program that happens to share an
executable. It prints the source format, the device it opened and a running
underrun count, and exits non-zero if there was even one — so it is usable as a
check and not only as something to watch.

The device is opened at the **file's** sample rate rather than a fixed 48 kHz.
Week 5 owns no resampler and the operating system's mixer already has a good
one, so nothing here has to resample and a 44.1 kHz file does not play sharp.
Week 6 needs a real one anyway: gapless playback across two files at different
rates cannot reopen the device between them.

Ogg Vorbis is not supported. miniaudio carries wav, flac and mp3 with it;
Vorbis needs a second decoder dropped in alongside, and the `Decoder` interface
makes that a new file rather than a change to an existing one.

### Indexing a music folder

```powershell
.\build\win-debug\bin\Debug\Sonora.exe --library "C:\Users\you\Music"
```

The folder is remembered, so later runs need no flag; every start rescans it,
which is cheap because the scan compares each file's modification time and size
against the index and reads the tags of nothing that has not changed. The index
lives in `%LOCALAPPDATA%\Sonora\library.sqlite` and is a cache: deleting it
costs one rescan and nothing else ([ADR 0007](docs/adr/0007-the-library-index-is-a-cache.md)).

There is no folder picker yet. A text box in the page would put a filesystem
path back on the bridge, which is precisely what week 7 removed; it needs a
native dialog, which belongs with the rest of the shell integration in week 9.

### Media keys and the system panel

Nothing to switch on: while a track is loaded, Sonora holds a system media
session. On Windows that is `SystemMediaTransportControls`, obtained for the
application window, so the volume overlay shows the title, the artist and the
cover art, its buttons work, and the keyboard's play/pause, next and previous
keys reach Sonora even when the window is minimised or another application has
focus.

What gets sent, and when, is decided by `sonora::core::MediaSessionPolicy` —
portable, clocked by an injected timestamp and unit tested. The player is
sampled ten times a second; the panel receives the metadata only when the track
changes, the position about once a second while playing, immediately after a
seek, and **nothing at all** while paused. The platform backend translates, and
decides nothing.

The macOS backend (`MPNowPlayingInfoCenter` + `MPRemoteCommandCenter`) is
written and compiles in CI, and **has never run**: no Mac has executed a line of
it. The file says so at the top, and names the three parts most likely to be
wrong. Linux is a stub that fails with a clear message.

### Working on the UI

Debug builds serve the UI from `ui/dist` on disk, so a UI change is a reload
rather than a C++ rebuild:

```powershell
cd ui
npm run dev      # vite build --watch
```

Release builds have no filesystem path at all and use the table embedded by
`tools/embed_assets.py`. The line Sonora prints at startup (`assets: ...`) says
which store is active.

### Presets

The presets do not pin a generator, so CMake picks the newest Visual Studio it
finds. Override with `$env:CMAKE_GENERATOR`.

| Preset | Builds |
|---|---|
| `win-debug`, `win-release` | everything, including the shell and CEF |
| `mac-release`, `linux-debug` | core, assets, platform and the tests — no CEF |

### Formatting

```powershell
./tools/format.ps1          # format in place
./tools/format.ps1 -Check   # what CI runs
```

clang-format is taken from `PATH`, and otherwise from the Visual Studio
installation — the C++ workload ships LLVM without putting it on `PATH`, so on
most Windows machines it is installed and unreachable by name at the same time.
CI uses **clang-format 18**; the script says so if the one it found is a
different major version, because two majors disagree about real cases and a
check that passes locally and fails in CI is worse than no check.

---

## Layout

```
src/core/        playback state, media-session policy — no OS, no screen, no sound card
src/audio/       ring buffer, decoders, engine — no OS, no CEF, no device
src/assets/      the web bundle as bytes: embedded table + the two stores that serve it
src/bridge/      the native<->web protocol: envelope, errors, generated dispatch — no CEF
src/platform/    iface/ + win/ + mac/ + linux/ — the only place #ifdef on the OS is allowed
src/shell/       the executable: window, CEF host, scheme handler, helper process
ui/              the web interface (TypeScript + Vite)
tests/           Catch2, runs against core and assets on every platform
schema/          bridge.schema.json — the single source of truth for the bridge
cmake/           CEF provisioning and pinning, asset and bridge generation
tools/           pin_cef.py, embed_assets.py, gen_bridge.py, make_icon.py, format.ps1, run-dev.ps1
docs/adr/        architecture decision records
```

Three decisions shape the rest:

- [ADR 0002](docs/adr/0002-platform-abstraction.md) — no conditional compilation
  on the operating system outside `src/platform/`. The process entry point lives
  there for exactly this reason. The macOS and Linux CI jobs build only the
  portable targets, so a leak turns the build red the week it happens.
- [ADR 0003](docs/adr/0003-serving-the-ui-over-a-custom-scheme.md) — the UI is
  served over `sonora://`, not `file://` and not a localhost server, and is
  embedded in the binary for release.
- [ADR 0004](docs/adr/0004-the-bridge-is-generated-from-a-schema.md) — the
  bridge is generated from one schema, and the generated handler interface is
  pure virtual, so the two ends cannot drift without breaking the build.
- [ADR 0006](docs/adr/0006-the-audio-callback-is-real-time.md) — the device
  callback allocates nothing, locks nothing and logs nothing, which is the exact
  opposite of the bridge's policy and deliberately so: each belongs to the
  thread it was written for.
- [ADR 0005](docs/adr/0005-events-are-pushed-and-coalesced.md) — events are
  pushed by the shell rather than carried on a persistent query, and the rate
  limiting that decides what the page sees lives in the portable target where it
  can be tested with an injected clock.
- The native loop stays in charge and CEF runs on an external message pump, so
  there is one message loop in the process rather than two fighting over it.

---

## License

MIT — see [LICENSE](LICENSE).
