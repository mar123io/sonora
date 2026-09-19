# ADR 0001 — Record architecture decisions

- **Status:** accepted
- **Date:** 2026-09-21

## Context

Sonora is built alone, over three months, in evening-sized pieces. The expensive
failure mode of a project like that is not writing the wrong code — it is
forgetting *why* the code is the way it is, and re-litigating a decision in week
nine that was already settled in week three.

There is a second audience. This repository exists to be read by people
deciding whether I can reason about systems, not only whether I can write C++.
A decision record shows the alternatives that were considered and rejected,
which source code cannot.

## Decision

Every decision that is expensive to reverse gets a short record in `docs/adr/`,
numbered, in Michael Nygard's format: context, decision, consequences.

A decision qualifies when reversing it would touch more than one subsystem, when
it constrains what can be built later, or when a reasonable engineer would
choose differently. Library choices, code style and anything a compiler can
enforce do not qualify.

Records are immutable. A decision that changes gets a new record that supersedes
the old one, and the old one is marked superseded rather than edited. The
history of a wrong turn is more useful than a clean file.

## Consequences

- Roughly one page of writing per significant decision, which is a real cost on
  an eight-hour week and is accepted deliberately.
- Design discussions in interviews can point at a document rather than a memory.
- The risk is ceremony: ADRs written for decisions nobody would question. The
  bar above exists to keep that in check.
