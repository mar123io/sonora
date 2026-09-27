# ADR 0009 — The update server is a signed file, not a service

- **Status:** accepted
- **Date:** 2026-11-30

## Context

The roadmap for this week says, in as many words:

> **Backend** `services/releases` (C++ with Drogon, or Go — choose and justify in
> the ADR): `GET /v1/manifest?channel=stable&version=X&platform=win-x64`

So this ADR is the justification it asked for, and the answer is neither of the
two languages offered.

It is worth writing down what the question actually is, because the shape of the
endpoint hides it. An updater needs one answer: *given a channel, a platform and
the version I am running, what should I install next, and how do I get it?* The
inputs are three enumerable values with a combined range in the dozens. The
answer changes exactly when somebody cuts a release, which for this project is a
few times a month and for a healthy desktop application is a few times a week.

A service computes that answer per request. A file computes it once, at release
time, for every input. Both are correct. The difference is everything that has to
exist around them: a process, a host, a deploy, a certificate, a health check, a
log, a bill, and a person who notices when it stops. And there is one asymmetry
that settles it — **a static file cannot be down while the CI run that produced
it was green.** The artefact and the thing that announces the artefact are the
same publish.

There is also a failure mode I would rather not build. An update server is the
one component of a desktop application that every installation talks to, on a
schedule, forever, including the installations from three years ago. A service
with a URL I control is a service I am promising to keep answering that URL, and
the honest expected lifetime of a hobby backend is shorter than the honest
expected lifetime of an installed copy of the program. Sparkle, Squirrel and
`electron-updater` all read static files, and not because their authors could not
write a web service.

## Decision

**The update server is a JSON file at a fixed URL, with a detached Ed25519
signature next to it.**

```
https://mar123io.github.io/sonora/updates/stable/manifest.json
https://mar123io.github.io/sonora/updates/stable/manifest.json.sig
```

The manifest names, for each platform, the newest version, where to get its full
package, where to get a delta from each of the recent versions, and the hash of
each of those things. The client fetches two files, verifies the second over the
first, and then decides. `src/update/manifest.h` holds the shape and the parser;
`schema/manifest.schema.json` holds the contract in the form a generator can
check itself against, the same way the bridge has done since ADR 0004.

Four rules make this a real design rather than a shortcut.

**Sign the bytes, not the object.** The signature covers the exact octets of
`manifest.json` as received. Verification happens *before* parsing, so the JSON
parser is never handed a byte that nobody signed. This is why the signature is
detached and why there is no `"signature"` field inside the document: a signature
inside the thing it signs means canonicalising the thing first, and
canonicalisation is a second parser with its own bugs, running before the
signature check, on unauthenticated input. Nothing in this project needs that.

**The manifest carries the hashes, so one signature covers every byte.** The
packages and deltas are not signed individually. They do not need to be: their
BLAKE2b-256 hashes are in the signed document, and the client refuses to unpack
or apply anything whose hash it has not already matched. One signature, one
private key, one verification, and no artefact reaches a parser before its hash
has been checked against an authenticated value.

**The trust root is a public key in the binary, not a certificate chain.** TLS
authenticates the host that served the file; the signature authenticates the
release. Only the second is depended on. A compromised CDN, a mis-issued
certificate or a corporate middlebox can stop Sonora from updating — which is
visible, recoverable and not a security failure — but cannot make it install
anything. The verifier holds a *list* of accepted keys rather than one, because
key rotation is otherwise a deadlock: learning a new key requires an update, and
that update has to be signed by a key that is already trusted.

**Ed25519, from libsodium, and no cryptography written here.** A 32-byte public
key fits on one source line where a human can compare it against the one in the
release tooling, verification is a single `crypto_sign_verify_detached`, and
libsodium's API makes the safe call the short one. The rule is absolute: this
repository contains no implementation of a cryptographic primitive.

## Consequences

- **The private key is a repository secret, and that is weaker than an offline
  key.** Whoever can run a workflow in this repository can sign a release. An
  offline key on a machine that never touches CI would be stronger, and it would
  also make signing a manual step — and a manual step in a release is a step that
  gets skipped on the release that matters. This is a trade I am making with my
  eyes open, and writing it here is the point: the failure mode is *compromise of
  the GitHub account is compromise of the update channel*, not *nobody thought
  about it*. A real product moves this to a hardware key and a release approval;
  the shape of the client does not change when it does.
- **No staged rollout today, and the shape for it is already right.** Releasing to
  10% of installations is a *number in the manifest*, not a behaviour of a server:
  the client hashes its own install id, compares against a threshold the manifest
  carries, and updates or does not. A static file serves data, and rollout is
  data. Week 12 adds the field; it does not add a service.
- **A kill switch is a publish.** Pointing `latest` back at the previous version
  stops the bleeding for everyone who has not updated yet, in the time it takes a
  workflow to run. What it cannot do is reach in and downgrade an installation
  that already took the bad version — that is what ADR 0011's rollback is for,
  and the two cover different halves of the same accident.
- **No per-installation telemetry from the update path.** The server learns
  nothing because there is no server; the CDN's logs see an IP and a URL. That is
  less than a service would know and more than nothing, and it means the answer
  to "how many people are on 1.0.1" is not available here. Week 12 owns that
  question and will have to answer it on purpose rather than as a side effect of
  serving updates.
- **The client must tolerate a manifest from the future.** `schema` is checked
  first and an unknown value is a clean refusal, not a parse attempt — the same
  discipline as the two SQLite schemas. An installation from three years ago will
  read a manifest written next year, and its most useful behaviour is to say "I do
  not understand this" and keep working.
- **Publishing is a second job in the release workflow**, and it runs after the
  MSI job, because a manifest that names a package nobody can download is worse
  than no manifest. The order is: build, sign, upload the artefacts, and only then
  publish the document that points at them.
