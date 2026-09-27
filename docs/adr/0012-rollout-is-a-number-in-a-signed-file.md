# ADR 0012 — Rollout is a number in a signed file, and the bucket is arithmetic

- **Status:** accepted
- **Date:** 2026-12-07

## Context

[ADR 0009](0009-the-update-server-is-a-signed-file.md) decided there is no update
service, and wrote down what that appeared to cost:

> **No staged rollout today, and the shape for it is already right.** Releasing to 10% of
> installations is a *number in the manifest*, not a behaviour of a server: the client
> hashes its own install id, compares against a threshold the manifest carries, and
> updates or does not. A static file serves data, and rollout is data. Week 12 adds the
> field; it does not add a service.

This is that field. The claim held, and the part worth an ADR of its own is not the
field — it is the two decisions underneath it: what the bucket is a function of, and where
the install id lives.

## Decision

**A release carries a rollout percentage. The client computes its own bucket from its
install id and the release's version, and takes the update only if the bucket is below the
percentage.**

```json
{
  "version": "0.6.1",
  "platform": "win-x64",
  "rollout": { "percent": 10 },
  "archive": { ... },
  "package": { ... }
}
```

Absent means 100: a release with no `rollout` goes to everybody, which is what every
release before this week did and what the `beta` channel does always. The `beta` channel
is not a client behaviour — it is a manifest that omits the field, and that is the whole
of it.

### The bucket is a function of the installation *and the version*

```
bucket = first 8 bytes of BLAKE2b-256(install_id ‖ version), little-endian, mod 100
```

Two properties fall out of that, and both were chosen rather than inherited.

**Widening a rollout only ever adds installations.** The bucket does not depend on the
percentage, so going from 10 to 25 keeps the first ten percent and adds fifteen more.
Nobody is ever dropped out of a rollout they were already in, which means "we widened it
and the crash reports stopped" is a real signal rather than a reshuffle.

**Every release reshuffles who goes first.** The version is in the hash, so being in the
first 10% of 0.6.1 says nothing about 0.6.2. The alternative — hashing the install id
alone — would make the same machines the early adopters of every release for as long as
they own the computer, which quietly turns a rollout into a permanent unpaid test group.
Spreading that across the population is worth one extra field in the hash.

### The install id is random, local, and never sent anywhere

Sixteen bytes from `randombytes_buf`, written once to
`<root>/Sonora.update/install-id` and read thereafter. Not a machine id, not a hardware
hash, not a user name, and not derived from anything: a machine that reinstalls gets a new
one, and that is correct, because what the number identifies is an installation and not a
person.

It is in `Sonora.update/` and not in the installation, for the reason
[ADR 0010](0010-the-delta-is-taken-over-an-uncompressed-archive.md) gives: the installed
tree has to contain exactly the members of the package it came from or the delta path
silently stops working. It therefore survives every update and every rollback, which is
what makes the bucket stable across them.

**Nothing transmits it.** The rollout decision happens entirely inside the client, against
a number in a file it already downloaded — so there is no cohort assignment to request, no
identifier in a query string, and nothing for a server to remember, because there is no
server. A staged rollout that needs to know who you are is a design choice, not a
requirement.

## Consequences

- **Rollout is advisory, not enforcement.** Anyone can read the manifest, see that 0.6.1
  exists, and install it by hand. That is fine: the field exists to limit how fast a bad
  release spreads by itself, not to stop somebody who wants it.
- **A held-back release does not hide an older one.** `ChooseUpdate` picks the newest
  release the installation is *eligible* for, so an installation outside 0.6.1's ten
  percent still gets 0.6.0 if that is newer than what it runs. This falls out of the same
  loop that skips refused versions, and it is tested with both filters at once, because
  two filters on one selection is exactly where an off-by-one lives.
- **Narrowing a rollout does nothing to installations that already updated.** The
  percentage governs who takes it next, and an update already applied is applied. Pulling a
  release back is [ADR 0009](0009-the-update-server-is-a-signed-file.md)'s kill switch —
  republish with `latest` pointing at the previous version — and an installation that
  already has the bad one is [ADR 0011](0011-the-swap-is-not-atomic-so-it-is-a-journal.md)'s
  rollback. Three mechanisms, three different moments, and none of them is a substitute for
  another.
- **The percentage is checked, not trusted to be sane.** `percent` outside 0..100 is a
  refusal of the whole manifest rather than a clamp: a generator that writes 1000 has a bug,
  and a client that quietly reads it as 100 is a client that hides it.
- **There is no way to ask an installation which bucket it is in** other than looking at
  the file on that machine. The about panel shows it, for the same reason it shows the
  refused list: a mechanism whose effect the user cannot observe is indistinguishable from a
  broken one.
