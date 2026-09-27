# Sonora

A native C++ desktop shell that hosts a web UI in CEF — with its own audio
engine, operating-system media integration, delta updates and a signed release
pipeline.

Sonora is built the way large desktop applications actually are: a native core
that owns audio and platform integration, a web layer that owns the interface,
and a versioned bridge between them so the two can ship independently.

> **Status: week 10 of 13.** The shell hosts a Chromium view, serves the UI over
> a custom `sonora://` scheme, and the two talk over a typed, versioned bridge
> generated from one schema. It plays music — a searchable library, a queue, and
> a gapless join between two tracks performed inside the device callback — and
> it behaves like a desktop application: the media panel and the media keys, a
> tray icon, taskbar thumbnail buttons, a jump list, one instance per session,
> `sonora://` links from the browser, and a window that reopens where you left
> it. It also ships: a tag builds an MSI on a clean machine and attaches it to
> a draft release, and every version it depends on is pinned. See
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
| Tray icon with menu, taskbar thumbnail buttons | working |
| Jump list of recently played albums | working (see the note on shortcuts below) |
| Single instance: a second launch hands its arguments over and exits | working |
| `sonora://track/<id>` and `sonora://album/<id>` from the browser | working |
| Window position, size and maximized state restored across restarts | working |
| Durable store: play history and the stable ids links are made of ([ADR 0008](docs/adr/0008-two-stores-with-opposite-policies.md)) | working |
| Per-user MSI: Start Menu shortcut with the AppUserModelID, clean uninstall | working |
| CI matrix windows/macOS/linux, green, with vcpkg and CEF cached | working |
| One version number, from the git tag, reaching the binary and the MSI | working |
| Every tool version pinned: CEF, vcpkg registry, clang-format, WiX | working |
| Code signing | demonstrated with a self-signed certificate — see below |
| Delta package, signed manifest, atomic swap with rollback | working — **84 KiB instead of 152 MiB** for three lines of C++ |
| Release signing key | **the development key; must be replaced before a real release** |
| Updater on macOS and Linux | the decisions are built and tested there; the five platform calls say no |
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

**Why the media panel used to say "Unknown app":** fixed by the installer in
week 10, and the reason is worth keeping because it explains what an installer
is actually for. Windows takes the *name and icon
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

Launched from the build folder, the panel says "Unknown app". Launched from the
Start Menu shortcut the MSI writes — which carries `System.AppUserModel.ID` —
it says **Sonora**, with the icon, and the jump list survives closing the
application. While developing, a shortcut made by hand does the same thing:

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

### Links, the tray and one instance

A second launch does not start a second Sonora. It hands its command line to the
one that is already running, raises that window, and exits — which is also what
makes a `sonora://` link from a browser open the player you already have rather
than a new one:

```powershell
start sonora://album/1     # play that album in the running copy
start sonora://track/7     # play that track
```

The scheme is registered for the current user at every start, pointing at the
executable that is running, so a rebuilt Sonora takes the links over from the
previous build instead of sending them to it.

The ids in those URLs are **durable ids**, issued by the second store
(`state.sqlite`), and that is not an implementation detail: the library index is
a cache whose row ids are reassigned every time it is rebuilt, while a jump-list
entry registered with Windows outlives the process, the index and several
releases. A URL carrying an index id would mean a different song after the next
rescan, silently. [ADR 0008](docs/adr/0008-two-stores-with-opposite-policies.md)
is the whole argument; the short version is that the two stores have opposite
policies and the boundary between them is the file path.

What the URL can say is one decimal number and nothing else: no percent-decoding,
no second path segment, no id that does not fit in an `int64`. It arrives from
outside the process — anyone can put a link in a web page — and it still has to
exist in the durable store, and then in the index, before anything happens.

The tray icon carries play/pause, previous, next and quit; the same four commands
are under the taskbar preview, on three buttons drawn by the code that installs
them. The jump list shows the last eight albums played.

**The jump list and the installed shortcut:** like the media panel's name (above),
a jump list is stored per AppUserModelID, and the shell takes that identity
seriously only when something registers it. Sonora claims the id
(`MarioLizzio.Sonora`, in `src/platform/win/app_identity.h`), which is enough for
the list to appear while it is running; week 10's installer writes the Start Menu
shortcut that carries the same id, and that is what makes it persist.

### Building the installer

```powershell
dotnet tool install --global wix --version 4.*
./tools/package.ps1              # build\packages\Sonora-<version>-x64.msi
./tools/package.ps1 -Sign        # ...and sign it, see below
```

Per-user, so it installs without administrator rights, into
`%LOCALAPPDATA%\Programs\Sonora`. Uninstalling removes the program and leaves
`%LOCALAPPDATA%\Sonora` alone: the two databases there are the play history and
the library cache, and an uninstaller that deletes somebody's listening history
has overstepped.

What ships is the `cmake --install` tree rather than the build directory, and
the dependency list is not written by hand — `install(RUNTIME_DEPENDENCY_SET)`
reads the binaries' import tables. The payload fragment WiX needs is generated
by `tools/gen_installer_files.py`, because the `Files` element that harvests a
directory in one line arrived in WiX v5, and v5 is behind the Open Source
Maintenance Fee licence gate. WiX is pinned to v4 for the same reason.

**Signing** is `tools/sign.ps1`: a certificate, `signtool`, a timestamp, and a
verification step that fails the build. Every one of those is the same in
production — but the certificate this repository can produce is self-signed,
which is trusted by exactly one machine, so signing is deliberately **not** in
CI. On a file other people download, a signature nobody can verify is worse
than none. A real release needs an OV or EV certificate with its key in an HSM
or a cloud signing service; the CI comment marks where that step goes.

### Versions, and why they are all pinned

One version number, and it comes from the git tag. `cmake/Version.cmake` runs
before `project()` and accepts only `vMAJOR.MINOR.PATCH` as a release tag — the
milestone tags (`v0.3-player`, `v0.4-native`) are names, not versions — and from
`project(VERSION ...)` the number reaches the executable's VERSIONINFO resource,
the startup line and the MSI's ProductVersion.

Four external things are pinned, and each one is pinned because leaving it
loose broke something:

| | Pinned in | What happened when it was not |
|---|---|---|
| CEF | `cmake/cef_version.cmake` | (week 2, pinned from the start) |
| vcpkg registry | `builtin-baseline` in `vcpkg.json` | two builds of one commit could link two different sqlite3 |
| clang-format | `CLANG_FORMAT_VERSION`, installed from pip | a check that passed locally and failed in CI |
| WiX | `WIX_VERSION`, `4.*` | the unpinned install took v7, which will not build without a commercial licence |

CI clones the vcpkg registry at the baseline rather than using the runner
image's copy. That is not belt and braces: vcpkg reads `versions/baseline.json`
from the pinned commit but the per-port version database from the working tree,
so a borrowed clone means a registry that half agrees with the pin.

### Where the window opens

Position, size and maximized state are remembered in a one-line file next to the
two databases, and the rule when reopening is one the tests are written against:
**the window always ends up entirely inside some display's work area.** A monitor
that was unplugged, a laptop undocked, a resolution that shrank, a settings file
edited by hand — none of them can put the window somewhere unreachable. Moving
between displays of different scale keeps its physical size rather than its pixel
count.

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

### Updating, and what an update is allowed to break

An installed copy of Sonora is 152 MiB, of which about 140 MiB is CEF. Changing three
lines of C++ produces a new 152 MiB installer, and asking everybody to download it again
is the problem this part exists to solve.

```
Sonora-0.6.0-win-x64.spk       213.1 MiB   the payload, uncompressed, never served
Sonora-0.6.0-win-x64.spk.zst    47.0 MiB   the same bytes, for downloading
0.5.0-0.6.0-win-x64.patch        84.0 KiB   from the previous version
```

Those are real numbers from the release job, and the last one is the point: **0.0385 % of
the package it rebuilds.**

#### How it works

There is no update server. The manifest is a signed JSON file attached to the newest
published release, at a URL that does not change:

```
https://github.com/mar123io/sonora/releases/latest/download/manifest.json
https://github.com/mar123io/sonora/releases/latest/download/manifest.json.sig
```

`sonora-updater.exe --check` fetches those two, verifies the Ed25519 signature **over the
manifest's bytes before any parser sees them**, and only then reads what it says. The
manifest carries a size and a BLAKE2b-256 hash for every artefact, so one signature covers
every byte that will ever be downloaded.

If there is a newer version, the updater packs the *installed* tree into an archive of its
own and checks that against the hash the manifest publishes for the version that is
running. If they match, it downloads the patch — 84 KiB — and applies it. If they do not
match, for any reason at all, it downloads the 47 MiB package instead. The delta path is an
optimisation with a checked precondition, never a correctness dependency.

Nothing moves until the application exits. Then:

```
Sonora/            the installation
Sonora.new/        the staged tree: unpacked, hashed, complete
Sonora.old/        the previous tree, kept until the new one has started
Sonora.update/     the journal, and the launch flag
```

A swap is two renames and a delete, and between the first two there is no installed copy of
Sonora at all — so every step is written to a journal before it is performed, and a process
starting up finishes or undoes whatever the journal describes.
[`docs/updater-states.md`](docs/updater-states.md) has the state machine, the table of
"journal says X, disk shows Y", and what to do by hand if an installation gets stuck.

The new version proves itself by starting: when its window is up and the page has loaded it
writes a flag naming its own version. There is no twenty-second timer anywhere — a deadline
in seconds measures the machine, not the program, and the machine least able to meet it is
the one where a good version would be thrown away. If the flag never appears, the start
*after* that one rolls the previous version back and adds the new one to a refused list, so
the same version is not downloaded and reverted every six hours forever.

#### Trying it without a release

Three things are worth running, and none of them needs a server:

```powershell
# What a release job produces, over any directory. Visual Studio puts the executable
# under bin/Release and Ninja under bin, so find it rather than guess:
$tool = (Get-ChildItem build/win-release -Recurse -Filter sonora_release.exe)[0].FullName
& $tool pack build/win-release/stage build/a.spk
& $tool compress build/a.spk build/a.spk.zst
& $tool delta build/old.spk build/a.spk build/a.patch
& $tool inspect build/a.spk

# The three tests from the roadmap -- install 1.0.0, be offered 1.0.1, verify; a corrupt
# delta; a version that will not start. Real trees, real signatures, real renames.
ctest --preset win-release -R "end to end"
```

`sonora_release delta` re-applies the patch it just produced and compares the bytes before
writing it out, because a patch that does not apply is a release that silently makes
everybody download 47 MiB and looks entirely healthy.

#### What is not done

- **The signing key in this tree is a development key.** Its private half has been outside a
  secret store, so it is not a signing key. `tools/update_keygen.ps1` makes a real one, and
  the release job refuses to publish a manifest while the binary still trusts the
  development one — a note in a comment is not a check. Until a key is in the repository
  secret, releases go out with no manifest, and installed copies see no updates.
- **The key lives in a repository secret**, which is weaker than an offline key: whoever can
  run a workflow here can sign a release. The trade is written down in
  [ADR 0009](docs/adr/0009-the-update-server-is-a-signed-file.md) rather than discovered
  later.
- **macOS and Linux have no updater.** Everything above `src/update/` is built and tested on
  both — the end-to-end tests run there too — and `shared/update_host_none.cpp` says no to
  the five platform calls rather than implementing four of them and shipping an application
  that looks like it can update itself.
- **If the new version's own updater is broken, nothing rolls back.** The decision is made
  by `Sonora.exe`, which is the binary whose ability to start is in question. Reinstalling
  the MSI is the documented recovery.

---

## Layout

```
src/core/        playback state, media-session policy, deep links, window placement — no OS
src/state/       the durable store: play history and stable ids (ADR 0008)
src/audio/       ring buffer, decoders, engine — no OS, no CEF, no device
src/assets/      the web bundle as bytes: embedded table + the two stores that serve it
src/bridge/      the native<->web protocol: envelope, errors, generated dispatch — no CEF
src/update/      manifest, signature, archive, patch, journal — the updater's decisions, no OS
src/platform/    iface/ + win/ + mac/ + linux/ + shared/ — the only place #ifdef on the OS is allowed
src/shell/       the executable: window, CEF host, scheme handler, helper process
src/updater/     sonora-updater: the one process allowed to move the installation
ui/              the web interface (TypeScript + Vite)
tests/           Catch2, runs against core and assets on every platform
schema/          bridge.schema.json — the single source of truth for the bridge
cmake/           CEF provisioning and pinning, asset and bridge generation
tools/           pin_cef.py, embed_assets.py, gen_bridge.py, make_icon.py,
                 gen_installer_files.py, gen_manifest.py, release_tool.cpp,
                 package.ps1, sign.ps1, update_keygen.ps1, format.ps1, run-dev.ps1
installer/       Sonora.wxs -- the MSI, and the shortcut that carries the AppUserModelID
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
- [ADR 0008](docs/adr/0008-two-stores-with-opposite-policies.md) — what the user
  did lives in a different store from what their files say, with the opposite
  durability policy, and the boundary between the two is the file path.
- [ADR 0009](docs/adr/0009-the-update-server-is-a-signed-file.md) — the update
  server is a signed static file and not a service, the signature covers the
  bytes rather than the parsed object, and the trust root is a public key in the
  binary rather than a certificate chain.
- [ADR 0010](docs/adr/0010-the-delta-is-taken-over-an-uncompressed-archive.md) —
  solid compression destroys a delta (measured: 128 KiB becomes 5 MiB), so the
  update artefact is an uncompressed archive, compression is transport only, and
  the client rebuilds its own archive from the files on its disk.
- [ADR 0011](docs/adr/0011-the-swap-is-not-atomic-so-it-is-a-journal.md) —
  replacing an installation is three operations and not one, so it is a journal
  and a pure decision function, tested by stopping the machine at every point at
  which it could stop.
- [ADR 0005](docs/adr/0005-events-are-pushed-and-coalesced.md) — events are
  pushed by the shell rather than carried on a persistent query, and the rate
  limiting that decides what the page sees lives in the portable target where it
  can be tested with an injected clock.
- The native loop stays in charge and CEF runs on an external message pump, so
  there is one message loop in the process rather than two fighting over it.

---

## License

MIT — see [LICENSE](LICENSE).
