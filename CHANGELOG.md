# Changelog

Notable changes, newest first. Versions follow [semantic versioning](https://semver.org/), and
the number comes from the git tag — see `cmake/Version.cmake`, which is also what decides that
a tag like `v0.9-delivery` is a milestone and not a release.

---

## Unreleased

Phase 6 of the roadmap, which the job description asked for by name: *"AI agent integrations
within the Spotify Desktop experience"*.

- **An agent gets a list, not a bridge.** 18 of the bridge's 27 methods are offered to an agent,
  by a line in the schema next to the method; the other 9 are absent from the only list the
  broker checks a plan against rather than hidden from the planner. 9 run on sight, 9 are
  proposed and wait for a person. The catalogue a planner is told about and the table the broker
  validates against are generated from the same block, so they cannot disagree.
- **The planner has no authority.** Every proposed call is validated against the catalogue and
  the parameter list, and a plan is admitted whole or not at all. A test drives a planner that
  deliberately obeys an instruction hidden in a track title, and the call is refused before
  anything is offered to anybody — because the tool is not on the list, not because the model
  resisted.
- **No provider is wired up**, and the interface is the deliverable. The planner that ships
  matches words and says so rather than pretending to understand a mood. ADR 0016 has the
  reasoning, what a real adapter must additionally do, and what the plan id is and is not.
- `src/agent/` has no CEF, no operating system, no network and no model in it, so every rule
  above is a unit test on all three platforms.

---

## 1.0.0

The first release meant for somebody else's computer. Thirteen weeks, and the point of it was
never the music player: it was to build the parts of a desktop application that are usually
left for later — the installer, the updater, the crash path, the performance gate — and to
find out what they actually cost.

### The application

- **A native window hosting a web interface in CEF**, with the native side owning the message
  loop and CEF running on an external pump. One loop in the process, not two.
- **Its own audio engine**: decoder → lock-free SPSC ring buffer → device callback, with the
  gapless join between two tracks performed *inside* the callback. wav, flac and mp3.
- **A library**: SQLite with FTS5, an incremental scan that reads nothing it has already seen,
  cover art, and a searchable, virtualised track list.
- **Windows integration**: the system media panel and the media keys with the window
  minimised, a tray icon, taskbar thumbnail buttons, a jump list of recent albums, one
  instance per session, `sonora://` links from the browser, and a window that reopens where it
  was left — including across a display that has been unplugged.
- **A typed bridge** between the two halves, generated from one schema, with capability
  negotiation and a degraded interface when something is unavailable. No file path ever
  crosses it.

### Shipping it

- **A per-user MSI** built by CI on a clean machine, with a Start Menu shortcut carrying the
  AppUserModelID that the jump list needs, and a clean uninstall.
- **Delta updates**: 86,053 bytes to move an installation from 0.5.0 to 0.6.0, against a
  49 MiB full package — 0.0385%. The update artefact is an uncompressed archive because solid
  compression destroys a delta by a factor of forty, which was measured before it was decided.
- **A signed manifest and no update server.** Ed25519 over the manifest's bytes, verified
  before any parser sees them; one signature covers every artefact because the manifest
  carries their hashes.
- **An atomic-swap installer with rollback.** Replacing a directory is three operations and not
  one, so every step is written to a journal before it is performed, and a version that never
  reports having started is rolled back by the start after it. There is no twenty-second timer
  anywhere in the project.
- **Staged rollout**: a percentage in the signed manifest and a stable bucket per installation,
  with the version inside the hash so that widening a rollout adds installations rather than
  reshuffling them.
- **Crash reporting**: CEF's own Crashpad, configured by a file beside the executable, five
  crash keys, and **the PDBs published with every release** — the one artefact that cannot be
  regenerated on the day it is needed.
- **A performance gate** that refuses a 10% regression on a pull request, and refuses to gate
  a metric whose median is not known to better than half that. Baselines keep every sample, so
  measuring for longer makes the gate tighter — and the week that shipped this found the limit
  of that idea: `ubuntu-latest` is a fleet, one commit measured 133 ms of cold scan on one host
  and 276 ms on another while the metric that counts bytes came back identical to the byte, and
  variance between hosts reached twice the regression the gate exists to catch. So a duration
  is gated where a person is reading it and reported everywhere else, a count is gated
  everywhere, and a benchmark that goes missing fails in every mode. ADR 0015.
- **Everything pinned**: CEF, the vcpkg registry commit, clang-format, WiX, Node. The same tag
  produces the same binaries on a runner and on a laptop.

### Known limitations

These are in the README in more detail, and they are here because a release that hides them is
a release that will disappoint somebody in week two.

- **macOS and Linux are compiled, not run.** The portable half — audio, bridge, library, state,
  update, benchmarks — is unit-tested on all three platforms in CI. The macOS window and media
  backends have never run on a Mac; the Linux backend is a stub that says so.
- **macOS and Linux have no updater.** The platform layer refuses the five calls it cannot
  make rather than pretending.
- **The CEF sandbox is off.** It is the largest debt in the project and it is recorded in
  ADR 0003 rather than buried.
- **Code signing is demonstrated with a self-signed certificate**, which is trusted by exactly
  one machine. SmartScreen will warn, and a real release needs a certificate from a CA.
- **If the new version's own updater is broken, nothing rolls back.** The decision is made by
  the binary whose ability to start is in question. Reinstalling the MSI is the documented
  recovery.
- **An update needs about 530 MiB free** for a 213 MiB installation: two payloads and a patch.
  The updater checks first and says so.
- **The durations in the performance table cannot be compared between machines**, which means
  three of the four metrics are measured and published rather than enforced. The open proposal
  is a calibrator per resource so that the gate can read a ratio instead of a millisecond; the
  prediction that it will not be tight enough for a 10% gate is written down in ADR 0015 before
  the experiment, along with the two cheaper fixes that the numbers already ruled out.

---

## 0.6.0

Delta updates, the signed manifest, the journal and the rollback. `sonora-updater.exe` arrives
as a separate executable, because the process that replaces an installation cannot be the
process running from it.

## 0.5.0

The MSI, the release pipeline, and one version number reaching the binary, the installer and
`--version` from the git tag.

## 0.4.0

Shell integration: tray, jump list, taskbar buttons, single instance, `sonora://` links, and a
window that remembers where it was.

## 0.3.0

The audio engine and the library: playback, the queue, gapless, the SQLite index and the
interface that uses them.

## 0.2.0

CEF inside the native window, the UI served over `sonora://`, and the generated bridge between
them.
