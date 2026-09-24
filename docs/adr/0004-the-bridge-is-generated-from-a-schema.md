# ADR 0004 — The native↔web bridge is generated from one schema

- **Status:** accepted
- **Date:** 2026-10-05

## Context

The shell and the interface are written in different languages, run in
different processes, and will be changed at different times. Every call between
them is a contract that exists twice: once in C++ and once in TypeScript.

Hand-written, that contract drifts. Someone renames a field on one side, the
other side keeps sending the old name, and nothing complains — JSON is happy to
carry a key nobody reads. The failure surfaces later, at runtime, as a value
that is quietly `undefined`. On a project where the UI layer is meant to move
independently of the native layer, that is not an edge case; it is the normal
state of affairs.

The alternatives considered:

1. **Hand-write both sides.** Fastest to start, and the drift above is
   guaranteed rather than likely.
2. **A generic dynamic bridge** — `invoke(name, anyObject)`, validated nowhere.
   No boilerplate at all, and no types, no completion, and no build-time signal
   when a method disappears.
3. **An existing RPC toolchain** (protobuf, Cap'n Proto, JSON-RPC libraries).
   Real schemas and real code generation, at the cost of a dependency in the
   renderer, a second type system to learn, and a wire format built for
   problems this project does not have. Twelve methods do not need gRPC.

## Decision

`schema/bridge.schema.json` is the single source of truth.
`tools/gen_bridge.py` reads it and emits, from the same description:

- C++ request and result structs, with parsing that checks every field;
- a **pure virtual** `BridgeHandlers` interface;
- the dispatch table;
- TypeScript interfaces and typed call wrappers.

The pure virtual interface is the load-bearing part. Adding a method to the
schema does not produce a TODO or a runtime "unknown method" — it stops the
build until someone implements it. Removing one breaks the call site. The two
ends cannot drift, because drifting does not compile.

Two supporting decisions follow from it:

**The bridge is a portable target.** `sonora_bridge` links nlohmann/json and
nothing else — no CEF. The transport moves two strings, a request in and a
response out, so everything that decides what a request *means* is testable
without a browser. That is why the macOS and Linux CI jobs run the protocol
tests, and why a protocol bug does not have to wait for someone on Windows to
notice it. The same reasoning produced `src/assets/` in week 2, and it has paid
for itself twice now.

**Handlers signal failure by throwing.** `BridgeError` carries a code from a
fixed, numbered enum; `Dispatch` catches it, catches JSON errors, and catches
anything else a handler throws, and turns all three into a typed response.
`Dispatch` itself is documented never to throw, which is not decoration: it is
called from inside a CEF callback that has no way to report an escaped
exception, and an exception crossing that boundary is undefined behaviour in
practice.

Exceptions here are a deliberate exception to a rule this project will adopt
elsewhere. The audio callback added in week 5 will forbid allocation, locks and
exceptions outright, because it runs under a deadline. The dispatch boundary
runs on the UI thread, is allowed to allocate, and benefits from an error path
that cannot be ignored by a caller who forgot to check a return value. Applying
one policy to both would be worse for one of them.

## Consequences

- **Python is a build dependency**, alongside CMake and Node. It already was,
  for CEF pinning and asset embedding.
- **Two build systems, one generator.** CMake owns the C++ output; npm owns the
  TypeScript, through a `generate` script that `prebuild` and `pretypecheck`
  depend on. Each generated file has exactly one owner, because two build
  systems writing the same path race each other.
- **The generated TypeScript is not committed.** It is a build product, and a
  committed one drifts from the schema the first time someone forgets to
  regenerate. The cost is that a fresh checkout cannot typecheck the UI until
  `npm run generate` has run once, which the npm scripts handle.
- **The type system is deliberately small**: string, int, double, bool, arrays
  of those, and optional fields. Nested structures are not supported yet. Week 7
  needs lists of tracks and will extend it; designing for that now would be
  designing without the problem in front of us. The generator validates the
  schema and refuses what it cannot express, so the limit is visible rather than
  silently mistranslated.
- **JSON's single number type does not leak in.** The schema distinguishes `int`
  from `double`, and `1.5` sent to an `int` field is rejected. Accepting it
  would make a rounding bug free to introduce.
- **Error codes are wire protocol.** They are numbered explicitly and a test
  pins the numbers. Renumbering them silently changes what an older UI thinks
  happened.
