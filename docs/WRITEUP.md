# Updating an application while somebody is using it

Sonora installs as 152 MiB, of which about 140 MiB is Chromium. Changing three lines of C++
produces a new 152 MiB installer, and asking everybody to download it again is the wrong
answer to a small change.

That is the easy half of the problem, and it is a compression problem. The hard half is that
**the files you have to replace are the files you are running from**, and there is no moment
at which a desktop application is not running from them — except one, and it is the moment
the user closes the window and stops paying attention.

Doing that safely took four measurements, one file format, a state machine with seven states,
and a second executable whose only job is to be somewhere else.

---

## The measurement that decided the design

I wrote the measurement before the code, because the whole week depended on a number I did
not have: how large is the difference between two builds that differ by three lines?

The test payload had the shape of the real one — a 137 MiB library standing in for
`libcef.dll`, a 631 KiB executable, a few files in subdirectories — and the second version
differed from the first by one string constant and one arithmetic expression. Then I took
`zstd --patch-from` between the two, packaged three ways:

| container | size to download | patch between v1 and v2 |
| --- | --- | --- |
| uncompressed archive | 213.1 MiB | **127.8 KiB** |
| per-file compressed (zip) | 64.5 MiB | 133.7 KiB |
| one solid compressed frame | 47.0 MiB | **5.04 MiB** |

The third row is the finding, and it is not the one I expected. It is not compression that
destroys a delta — the middle row compresses every file and the patch barely grows. It is
**solid** compression: one stream over the whole payload, where a single changed byte shifts
the entire remaining output and there is nothing left for a patcher to match against. Forty
times the patch, for 17 MiB saved on the full download.

Two consequences fell out of it immediately, and both contradicted the plan.

**The installer cannot be the thing that gets patched.** An MSI with `EmbedCab="yes"` is the
third row of that table: one compressed stream containing everything. The obvious design —
publish MSIs, patch between them — was dead before any code existed.

**So the update artefact is an uncompressed archive of the payload**, compressed only for
transport. The format (`.spk`) is deliberately boring: a magic number, a count, then for each
member a path, a size, a BLAKE2b-256 hash, flags, and the bytes. Members in a strictly
increasing path order, no compression, no timestamps, no attributes except one execute bit.

That last part matters more than it looks, and it is the second reason the format is boring:
**a client can rebuild its own `.spk` from the files on its disk, byte for byte.** There is no
compressor in the path, so there is nothing whose version could change the output. That is
what makes a delta verifiable at both ends — the client hashes the archive it just rebuilt
from its own installation and checks it against the hash the manifest publishes for the
version it is running. If they match, the patch is guaranteed to apply to the same bytes it
was computed from. If they do not match, for any reason at all, the client downloads the full
47 MiB instead.

**The delta path is an optimisation with a checked precondition, never a correctness
dependency.** Everything downstream of that sentence got simpler.

There was a fourth measurement, and it is the kind of thing you only find by looking. The
same compression level produced an 84.3 KiB patch single-threaded and **178.9 KiB with the
tool's own default thread count** — and non-monotonically in level, so tuning by eye would
have made it worse. Multithreaded compression cuts the input into independently compressed
jobs, and a patch is made of nothing except long-range matches. So `ZSTD_c_nbWorkers = 0`, in
a named constant, with the two numbers in the comment beside it, because the next person to
"speed up the release job" needs to see what that costs in the same diff.

---

## There is no update server

The roadmap asked for a small service: `GET /v1/manifest?channel=…&version=…&platform=…`,
answering with a target version, a URL, a hash and a signature.

The question it answers has three enumerable inputs and an answer that changes only when
somebody cuts a release. That makes it a file. And there is one asymmetry that settles it
rather than making it a preference: **a static file cannot be down while the CI run that
produced it was green.** A service can be down, needs a certificate that expires, needs
somebody to notice when it stops, and needs to stay up for as long as the oldest installation
keeps asking — which is forever.

So the manifest is a JSON file attached to the newest release, at a URL that does not change,
and next to it a detached signature.

Trust is the part worth being careful about, because an updater is the one component whose
compromise is total: it replaces the application's own binaries. Three decisions:

**The signature covers the bytes, not the parsed object.** `sonora-updater` verifies the
Ed25519 signature over the manifest's octets *before any parser sees them*. A JSON parser is
a few thousand lines of C++ that has to be correct on input chosen by whoever can write to
that URL; putting the signature check in front of it means the parser only ever runs on bytes
a private key has already vouched for.

**One signature covers every byte that will ever be downloaded**, because the manifest carries
a size and a BLAKE2b-256 hash for every artefact. The packages themselves are not signed and
do not need to be.

**The trust root is a public key compiled into the binary**, not a certificate chain. There is
no revocation, no expiry and no third party — which is a real limitation, honestly recorded
rather than discovered later: rotating the key means shipping a build that trusts the new one,
and the release job refuses to publish a manifest while the binary still trusts the
development key, because a note in a comment is not a check.

One accident worth keeping: the release is signed by **OpenSSL on a Linux runner** and
verified by **libsodium on somebody's laptop**. Two implementations of Ed25519, and the only
way to know they agree is to have one check a signature the other made — so a unit test
verifies a signature that the exact release command produced.

---

## The swap is not atomic, so it is a journal

Here is the part that took the longest, and it is the part that has nothing to do with
compression.

Replacing an installed directory is three operations:

```
rename  Sonora      -> Sonora.old
rename  Sonora.new  -> Sonora
delete  Sonora.old
```

Each rename is atomic. **The sequence is not.** Between the first and the second there is no
installed copy of Sonora on the machine at all, and that window is where the laptop's battery
dies, where the user holds the power button, where the antivirus takes an exclusive lock on a
file it has decided to scan.

It is tempting to look for a primitive that avoids this. There isn't one: `ReplaceFile` works
on files and not on trees, the transactional NTFS API was deprecated, and keeping both trees
installed only moves the question to which one gets started — the same problem plus a second
copy of Chromium.

So the answer is not to make the sequence atomic. It is to make every intermediate state
recognisable and recoverable:

```
Sonora/            the installation
Sonora.new/        the staged tree: unpacked, hashed, complete
Sonora.old/        the previous tree, kept until the new one has started
Sonora.update/     the journal, and the launch flag
```

**The journal is written before the step it describes, not after.** Seven stages — `idle`,
`staging`, `staged`, `moving-out`, `moving-in`, `unconfirmed`, `rolling-back` — and the
decision about what to do next is a pure function:

```cpp
Step NextStep(const Journal& journal, const Facts& facts);
```

`Facts` is four booleans: does `Sonora` exist, does `Sonora.new` exist, does `Sonora.old`
exist, what does the launch flag say. Nothing in that function touches a disk, which is the
entire point — it means the interesting half of an updater can be tested by *enumeration*
rather than by hoping.

And it was. The test walks every state: 7 stages × 8 combinations of the three directories ×
3 things the flag can say × 2 attempt counts × 2 launch outcomes, each carried to completion
across several simulated sessions, including the combinations that "cannot happen" — because
"cannot happen" is exactly the assumption that breaks on a laptop with a half-finished
rollback.

That test found a real bug before any user could: with an idle journal, no `Sonora`, and a
`Sonora.old` present — which is what a power cut between the two renames leaves behind — the
function I had written would have **deleted the only remaining copy of the application.** It
is the kind of mistake that is invisible in a code review and obvious in a table.

One more thing follows from all of this: **the process that moves the directories cannot be
the application.** `Sonora.exe` cannot rename the directory it is loaded from. So
`sonora-updater.exe` is a separate executable, it copies *itself* to `%TEMP%` before it
touches anything, and it waits for the application's process to exit. The shell's entire part
in an update is four calls: recover at startup, start a periodic check, write a flag when the
window is up, and hand over on the way out. Nothing in the application moves a file.

---

## Proving that the new version works

A rollback needs an answer to "did it start?", and the roadmap suggested the obvious one: the
new version writes a flag within twenty seconds.

**A deadline in seconds measures the machine, not the program.** Twenty seconds is generous on
my desktop and tight on a four-year-old laptop that is also running a virus scan and a Windows
update — and the machine least able to meet the deadline is exactly the machine where a
perfectly good version would be thrown away and replaced with an older one. That is a rollback
loop caused by a stopwatch.

So the flag is written at a **milestone** instead: when the window is on screen and the main
frame has finished loading. There is no timer anywhere in the project. If the flag never
appears, the *next* start rolls back — and the version that failed goes on a refused list, so
the same broken version is not downloaded and reverted every six hours forever.

`kMaxLaunchAttempts = 1`, which is the other half of that decision: a version that failed to
start once does not get a second chance from a mechanism that cannot tell "crashed" from
"the user killed it during startup".

---

## What it produced, and what is still missing

From the shipped release job, on the real payload:

```
Sonora-0.6.0-win-x64.spk       223,410,842 B    the payload, uncompressed, never served
Sonora-0.6.0-win-x64.spk.zst    49,320,420 B    the same bytes, for downloading
0.5.0-0.6.0-win-x64.patch            86,053 B    from the previous version
```

**0.0385% of the package it rebuilds.** And `sonora_release delta` re-applies the patch it has
just produced and compares the bytes before writing it out, because a patch that does not
apply is a release that silently makes everybody download 47 MiB and looks entirely healthy.

The honest remainder:

- **If the new version's own updater is broken, nothing rolls back.** The rollback decision is
  made by `Sonora.exe`, which is the binary whose ability to start is in question. If it does
  not reach its first ten lines, the journal is never read. This is the residual risk of every
  in-place updater without a permanently resident service, and the mitigation is the one that
  already exists: the MSI is still on the release page, and reinstalling it is a documented
  recovery.
- **macOS and Linux have no updater.** Everything above the platform layer is built and tested
  on all three — the end-to-end tests run there too — and the platform stubs say "no" to the
  five calls they cannot make, rather than implementing four of them and shipping an
  application that looks like it can update itself.
- **An update needs room for two payloads and a patch**: about 530 MiB free for a 213 MiB
  installation. The updater checks first and says so, instead of failing halfway.

---

## What I would keep

**Measure before deciding.** The solid-compression number changed the artefact, the container
and the client's job, and it cost an afternoon. Taken on faith it would have cost a release.

**Separate the decision from the effect.** `NextStep` decides and returns; something else acts.
That one seam is why a crash-safety property could be enumerated instead of tested by pulling
the plug on a laptop thirty times.

**Write down what the code cannot check.** The code enforces what it can; everything else is a
sentence somebody will read before they change it.

---

Related: [ADR 0009](adr/0009-the-update-server-is-a-signed-file.md) ·
[ADR 0010](adr/0010-the-delta-is-taken-over-an-uncompressed-archive.md) ·
[ADR 0011](adr/0011-the-swap-is-not-atomic-so-it-is-a-journal.md) ·
[the state machine, and how to recover an installation by hand](updater-states.md)
