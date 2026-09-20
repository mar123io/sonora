# Sonora

A native C++ desktop shell that hosts a web UI in CEF — with its own audio
engine, operating-system media integration, delta updates and a signed release
pipeline.

Sonora is built the way large desktop applications actually are: a native core
that owns audio and platform integration, a web layer that owns the interface,
and a versioned bridge between them so the two can ship independently.

> **Status: week 2 of 13.** The shell hosts a Chromium view that loads the UI
> over a custom `sonora://` scheme. There is no bridge between them yet.
> See [ROADMAP.md](ROADMAP.md) for what lands when.

---

## What is built

| Area | State |
|---|---|
| Native window (Win32, per-monitor v2 DPI) | working |
| CEF embedded, separate helper process, external message pump | working |
| `sonora://app` custom scheme, embedded UI bundle | working |
| DevTools on F12, debug builds only | working |
| Playback state machine, asset store — unit tested | working |
| Platform abstraction, macOS backend | written, compiled in CI, **not tested on hardware** |
| Platform abstraction, Linux backend | stub; fails with a clear message at runtime |
| Typed native↔web bridge | weeks 3-4 |
| Audio engine, gapless playback | weeks 5-6 |
| Local library + real UI | week 7 |
| SMTC, media keys, tray, jump list | weeks 8-9 |
| MSI packaging, CI matrix | week 10 |
| Delta updater with signature + rollback | week 11 |
| Staged rollout, crash reporting, perf gates | week 12 |

Performance numbers go here in week 12, together with the script that
reproduces them. Until then this table is the honest version.

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

---

## Layout

```
src/core/        playback state — no OS dependency, no screen, no sound card
src/assets/      the web bundle as bytes: embedded table + the two stores that serve it
src/platform/    iface/ + win/ + mac/ + linux/ — the only place #ifdef on the OS is allowed
src/shell/       the executable: window, CEF host, scheme handler, helper process
ui/              the web interface (TypeScript + Vite)
tests/           Catch2, runs against core and assets on every platform
cmake/           CEF provisioning and pinning, asset embedding
tools/           pin_cef.py, embed_assets.py, format.ps1
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
- The native loop stays in charge and CEF runs on an external message pump, so
  there is one message loop in the process rather than two fighting over it.

---

## License

MIT — see [LICENSE](LICENSE).
