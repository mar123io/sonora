# ADR 0003 — The UI is served over a custom `sonora://` scheme, embedded in the binary

- **Status:** accepted
- **Date:** 2026-09-28
- **Supersedes:** nothing

## Context

The shell hosts a web interface in CEF. That interface has to be loaded from
somewhere, and the choice decides the page's origin, which in turn decides what
the web platform will let it do for the rest of the project's life.

Three options:

1. **`file://`** — point the browser at an `index.html` next to the executable.
   Simple, and wrong in ways that surface late: `file://` is an opaque origin,
   so there is no `localStorage`, no service workers, no `fetch` of sibling
   files without disabling web security, and `window.isSecureContext` is false,
   which locks out most modern APIs. The usual fix is `--allow-file-access-from-files`
   and `--disable-web-security`, which is to say: turn off the sandbox that
   makes hosting a web engine survivable.

2. **A local HTTP server** — bind to `127.0.0.1` on some port and load from
   there. A real origin with all its capabilities, at the cost of a listening
   socket on the user's machine. That socket is reachable by every other
   process on the box and by any page the user has open. It also needs port
   negotiation, a firewall prompt on first run, and an answer for "what happens
   when the port is taken".

3. **A custom scheme backed by a resource handler** — register `sonora://` with
   CEF and serve the bytes from the native layer.

## Decision

`sonora://app/` is registered with `STANDARD | SECURE | CORS_ENABLED |
FETCH_ENABLED`, and a `CefResourceHandler` serves the bundle.

- `STANDARD` gives the scheme a real, tuple-based origin, so `sonora://app` has
  storage and a meaningful same-origin policy.
- `SECURE` makes Chromium treat it as a trustworthy origin, which is the gate
  in front of most of the modern web platform.
- No socket is opened, so nothing outside the process can reach the UI.

In release builds the bundle is compiled into the executable by
`tools/embed_assets.py`. Embedding means the UI is covered by the binary's code
signature: an attacker who can write files next to the executable cannot swap
the interface without invalidating the signature. It also removes a class of
installer bugs where the binary and its assets get out of step.

In debug builds the same scheme is served from `ui/dist` on disk instead, so a
UI change is a reload rather than a C++ rebuild. The origin is identical in both
modes, which matters: a difference in origin between development and release is
a difference in what the web platform permits, and that is the last place anyone
wants a surprise.

The handler sets a strict `Content-Security-Policy` that allows only `'self'`.
The UI is code we shipped; it has no business reaching the network directly, and
anything it needs from outside should go through the native layer, which is the
entire point of having one.

## Consequences

- **The sandbox is currently off.** `cef_sandbox.lib` is published only for the
  static CRT, while vcpkg's `x64-windows` triplet and the CEF DLL wrapper use
  the dynamic one; mixing CRTs in a single binary is not a configuration, it is
  a crash waiting for a release build. Turning the sandbox off is the wrong
  trade for a shipping product and an acceptable one for week 2. Settling on a
  single CRT across every dependency, and turning it back on, belongs in the
  packaging work of week 10. This is written down rather than left in a comment
  because it is the kind of thing that quietly never gets fixed.
- A custom scheme is unfamiliar to anyone reading the code for the first time,
  and DevTools shows origins they have not seen before. The gain in capability
  and the closed attack surface are worth the explanation.
- Debug and release resolve assets through different code paths, so "works in
  debug" does not prove the embedded path works. The `assets:` line printed at
  startup says which store is active, and a release build is the only thing that
  proves the embedded table.
- Adding a file to the UI means rebuilding the C++ for release. Acceptable: the
  UI is built once per release, not once per edit.

## One sanctioned exception to ADR 0002

`src/shell/cef/runtime.cpp` contains a single `#if defined(_WIN32)`, around the
construction of `CefMainArgs`. That type genuinely has a different shape per
platform — `HINSTANCE` on Windows, `argc`/`argv` elsewhere — because CEF's API
does, not because our code does. Wrapping it in yet another abstraction would
hide one line of honest platform difference behind a layer nobody would thank us
for. Everything else platform-specific stays in `src/platform/`, including the
process entry point, which moved there for exactly this reason.
