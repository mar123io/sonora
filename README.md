# Sonora

A native C++ desktop shell that hosts a web UI in CEF — with its own audio
engine, operating-system media integration, delta updates and a signed release
pipeline.

Sonora is built the way large desktop applications actually are: a native core
that owns audio and platform integration, a web layer that owns the interface,
and a versioned bridge between them so the two can ship independently.

> **Status: week 1 of 13.** The window opens and the test suite is green.
> Nothing else works yet. See [ROADMAP.md](ROADMAP.md) for what lands when.

---

## What is built

| Area | State |
|---|---|
| Native window (Win32, per-monitor v2 DPI) | working |
| Playback state machine + unit tests | working |
| Platform abstraction, macOS backend | written, compiled in CI, **not tested on hardware** |
| Platform abstraction, Linux backend | stub; fails with a clear message at runtime |
| CEF embedding, `sonora://` scheme | week 2 |
| Typed native↔web bridge | weeks 3-4 |
| Audio engine, gapless playback | weeks 5-6 |
| Local library + UI | week 7 |
| SMTC, media keys, tray, jump list | weeks 8-9 |
| MSI packaging, CI matrix | week 10 |
| Delta updater with signature + rollback | week 11 |
| Staged rollout, crash reporting, perf gates | week 12 |

Performance numbers go here in week 12, together with the script that
reproduces them. Until then this table is the honest version.

---

## Building

### Prerequisites

| | |
|---|---|
| **MSVC** | Visual Studio 2022 or newer, or Build Tools, with the **Desktop development with C++** workload. The Windows SDK comes with it and is required. |
| **CMake** | 3.28 or newer. `cmake --version` |
| **vcpkg** | Any recent checkout, with `VCPKG_ROOT` pointing at it. |

Installing vcpkg, once:

```powershell
git clone https://github.com/microsoft/vcpkg C:\dev\vcpkg
C:\dev\vcpkg\bootstrap-vcpkg.bat
[Environment]::SetEnvironmentVariable('VCPKG_ROOT', 'C:\dev\vcpkg', 'User')
```

`SetEnvironmentVariable` persists the variable; open a **new** terminal
afterwards, because the running one does not inherit it.

### Build

```powershell
cmake --preset win-debug
cmake --build --preset win-debug
ctest --preset win-debug
```

The first configure takes a few minutes: vcpkg builds Catch2 from source.
The executable lands in `build/win-debug/bin/Debug/Sonora.exe` with a Visual
Studio generator, or `build/win-debug/bin/Sonora.exe` with Ninja.

The presets do not pin a generator, so CMake picks the newest Visual Studio it
finds and the build does not break when the toolchain is upgraded. To force one:

```powershell
$env:CMAKE_GENERATOR = 'Ninja'   # from a Developer Command Prompt
```

Debug builds use the console subsystem so `stdout` is visible while developing;
release builds use the windowed subsystem. Both entry points are compiled and
the linker picks the one matching.

### CEF

CEF is not downloaded by default — a week-1 checkout should not pay for a 1 GB
archive to build a window. When week 2 starts:

```powershell
python tools/pin_cef.py --platforms windows64 macosarm64
cmake --preset win-debug -DSONORA_ENABLE_CEF=ON
```

`pin_cef.py` resolves the current stable build, downloads it once, verifies the
SHA-1 published upstream, and records a locally computed SHA-256 in
`cmake/cef_version.cmake`. That file is committed: the build never resolves
"latest" at configure time.

### Formatting

```powershell
./tools/format.ps1          # format in place
./tools/format.ps1 -Check   # what CI runs
```

---

## Layout

```
src/core/        playback state, library model — no OS dependency, no screen, no sound card
src/platform/    iface/ + win/ + mac/ + linux/ — the only place #ifdef on the OS is allowed
src/shell/       the executable: window, and from week 2 the CEF host
tests/           Catch2, runs against core on every platform
cmake/           CEF provisioning, pinned versions
tools/           pin_cef.py, format.ps1
docs/adr/        architecture decision records
```

The rule that shapes everything else is in
[ADR 0002](docs/adr/0002-platform-abstraction.md): no conditional compilation on
the operating system outside `src/platform/`. The macOS and Linux CI jobs exist
to enforce it — they build only what is supposed to be portable, so a leak turns
the build red the week it happens.

---

## License

MIT — see [LICENSE](LICENSE).
