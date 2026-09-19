# ADR 0002 — One interface per platform capability, no conditional compilation outside `src/platform/`

- **Status:** accepted
- **Date:** 2026-09-21

## Context

Sonora targets Windows first, with macOS as a second target and Linux as a
possibility. Development and manual testing happen on Windows only. The obvious
way to write cross-platform C++ is to write the Windows code and sprinkle
`#ifdef _WIN32` where the other platforms will eventually differ.

That approach fails in a predictable way. The conditionals spread from the
window code into the audio code, then into the library scanner, then into
anything that touches a path. By the time a second platform is attempted, the
"portable" code is a Windows program with holes in it, and porting means
rewriting rather than implementing.

The risk is sharpest here because the second platform will not be exercised.
A rule that is only checked by good intentions is not a rule.

## Decision

Every capability that differs per operating system is expressed as an abstract
interface under `src/platform/iface/`, with one implementation directory per
platform (`win/`, `mac/`, `linux/`). CMake selects the sources; the calling code
never knows which one it got.

`#ifdef` on the host platform is allowed **only** inside `src/platform/<os>/`.
It is not allowed in `src/core/`, `src/bridge/`, `src/shell/` or `tests/`.

Two things enforce this rather than discipline alone:

1. `sonora_core` links no platform library. It is the target that holds
   playback state, the library model and, later, the bridge payloads.
2. CI builds on macOS and Linux from week one. Those jobs build only the
   portable targets. Anything platform-specific that escapes `src/platform/`
   breaks them immediately.

The `Window` interface is deliberately minimal — show, close, native handle,
scale factor, two callbacks. An interface that is cheap to implement is one that
gets implemented on the second platform. Every method added to it is a method
that must be written three times.

## Consequences

- A small indirection cost on every platform call. Irrelevant at the rate these
  are called; nothing on the audio path goes through a virtual interface.
- The macOS implementation is written but never run. It can be wrong in ways
  compilation does not catch, and the README says so plainly rather than
  implying a tested port.
- The Linux implementation is a stub that fails with a readable message at
  runtime instead of failing to link. Keeping it compilable is what makes the
  Linux CI job a useful portability check rather than a dead target.
- Adding a capability now costs an interface plus three implementations. That
  friction is intentional: it is the mechanism that keeps the abstraction honest
  rather than a folder naming convention.
