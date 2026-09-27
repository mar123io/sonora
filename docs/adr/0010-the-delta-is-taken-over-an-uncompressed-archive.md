# ADR 0010 — The delta is taken over an uncompressed archive, and the installer is not the update

- **Status:** accepted
- **Date:** 2026-11-30

## Context

Week 10 produced an MSI of 152 MiB, of which about 140 MiB is CEF. The
observation that opens this week is that a change of three lines of C++ produces
a new MSI of 152 MiB, and that asking everybody to download it again is the
problem this week exists to solve.

The obvious move is to delta the installer: `zstd --patch-from=old.msi new.msi`,
publish the patch, and let the client rebuild the new MSI locally. I measured it
before writing any code, because the whole week's design hangs on the number.

The experiment is a 213 MiB payload tree standing in for Sonora's — a 137 MiB
library in place of `libcef.dll`, a 33 MiB blob in place of the snapshot, a 41 MiB
one in place of SwiftShader, five locale files, and a 631 KiB executable. The
second version differs from the first by one string constant and one arithmetic
expression in the executable — three lines of C++, and 472 KiB of the binary's
bytes changed, because everything after the edit shifted by eight bytes. Then the
same payload in three container shapes, and a `--patch-from` between each pair:

| Container | Its own size | Delta v1 → v2 | Delta as % of container |
|---|---|---|---|
| Store-only archive (no compression) | 213.1 MiB | **127.8 KiB** | 0.06 % |
| Per-file compressed (`zip -9`) | 64.5 MiB | **133.7 KiB** | 0.20 % |
| Solid compressed (one `zstd -19` frame) | 47.0 MiB | **5.04 MiB** | 10.7 % |

*(All three with the same command: `zstd -9 --single-thread --long=28
--patch-from`. The choice of level and of one thread is itself a measurement; see
the last consequence below.)*

The result is sharper than the guess I started with. It is not compression that
destroys a delta — it is **solid** compression. A container that compresses each
member independently leaves every unchanged member byte-identical, so the patch is
still about the size of one changed file. A container that compresses the whole
payload as a single stream re-encodes everything downstream of the first changed
byte, and a 128 KiB patch becomes a 5 MiB one: forty times worse, for a change of
three lines.

An MSI with `MediaTemplate EmbedCab="yes"` is the third row. The cabinet is one
compressed stream across the files in it, so the installer is the one shape of the
payload that must not be the thing we patch.

There is a second reason that needs no measurement. Applying an update by running
an installer means asking Windows Installer to perform the swap: it decides when
files move, it holds the transaction, it owns the rollback, and it cannot replace
files belonging to a process that is running. Every property ADR 0011 is about —
when the swap happens, what happens if the machine loses power during it, whether
the previous version is still on disk afterwards — would become MSI semantics
instead of ours. The installer's job is the first install. It is a bad update
mechanism even when the bytes are cheap.

## Decision

**The update artefact is a store-only archive of the payload. Compression is
transport only, and the client rebuilds its current archive from the installed
files rather than keeping a copy.**

Concretely:

1. A release publishes `Sonora-<version>-win-x64.spk` — the payload's files
   concatenated behind a header, in sorted path order, **uncompressed**. This is
   the thing hashes are taken of, the thing patches are computed against, and the
   thing that gets unpacked.
2. It is *served* as `Sonora-<version>-win-x64.spk.zst`, a single zstd frame over
   those same bytes. 213 MiB becomes 47 MiB on the wire at level 19, and the
   client decompresses it back to the exact bytes the hash in the manifest names.
   Compression appears nowhere in the reproducibility argument, because the hash
   is of the decompressed archive and decompression is exact by definition.
3. A delta from version X is a `--patch-from` between X's `.spk` and this one's.
4. **The client reconstructs its own installed version's `.spk` from the files on
   disk** and checks that hash against the manifest before patching. It does not
   keep the archive it was installed from, and the installer does not ship one:
   doubling the size of a 152 MiB installer to make later updates cheap is a bad
   trade for the person installing for the first time.

Step 4 is the load-bearing one and the reason the archive is store-only. To
reconstruct a byte-exact archive from an installed tree, everything about the
format has to be decided by the tree's contents: the member order is the sorted
paths, the header fields are the sizes and hashes of the files, and there is *no
compressor in the path* — so no compression level, no zstd version, no thread
count, nothing that could make the same files produce different bytes on a
different machine in a different year. Week 10's pinning is what makes a build
reproducible; this is the same argument applied to an artefact that has to be
reproducible on a stranger's laptop, where nothing is pinned at all.

And it verifies itself. If the reconstruction does not hash to what the manifest
says — a file edited by a virus scanner, a half-finished previous update, a
version this code was never told about — the client does not guess. It downloads
the full package. The delta path is an optimisation with a checked precondition,
never a correctness dependency.

The format is written down in `src/update/archive.h`: an eight-byte magic, a
version, a count, then one header per member — path, size, BLAKE2b-256 of its
contents — and then the members' bytes. No offsets, so it is read in one forward
pass with bounded memory; no timestamps, no permissions beyond one bit, no
compression, nothing that differs between two machines holding the same files.

## Consequences

- **Reading an archive is reading untrusted input, and it is treated as such.**
  Member paths are rejected unless they are relative, `/`-separated, free of `.`
  and `..` segments, free of backslashes, colons and control characters, and under
  1024 bytes; two members may not name the same path; the declared sizes must add
  up to exactly the bytes present. The hash was already checked against a signed
  manifest by the time the parser runs, so these checks are defence in depth — and
  a path check after a signature check is not redundant, it is the difference
  between trusting the release process and trusting it absolutely.
- **An update needs room for two payloads plus a patch.** Reconstructing the old
  archive, writing the new one and unpacking it peaks at roughly 2.5× the payload:
  about 530 MiB free for a 213 MiB installation. The updater checks free space
  before it starts and says so plainly rather than failing halfway, and it deletes
  the reconstruction as soon as the patch has been applied.
- **Applying a patch costs about 430 MiB of RAM and under a second.** zstd's
  `--patch-from` holds the whole prefix in memory and needs a window large enough
  to reach back across it: `windowLog` 28 for a 213 MiB archive. That is a real
  number on a small machine, it happens in a separate process that exits, and it
  is measured rather than assumed. Generating one costs more — around 820 MiB and
  seventy seconds — and that happens on a CI runner.
- **The full package is a 47 MiB download, not 213 MiB.** This matters more than it
  looks: the fallback path is taken on the first update after any reconstruction
  mismatch, and a fallback nobody can afford is not a fallback.
- **Two artefacts per release instead of one**, and the MSI is no longer the only
  thing the release job produces. The MSI installs; the `.spk.zst` updates. They
  are built from the same staging directory in the same job, so they cannot
  disagree about what version 1.0.1 contains — and CI checks exactly that by
  reconstructing the `.spk` from the MSI's own payload directory and comparing
  hashes.
- **The delta is generated for the last few versions only.** Every published
  version needs its own patch against the new one, so the cost is linear in how
  many we promise; the manifest lists the ones that exist and the client that finds
  none takes the full package. Three is the number today, and it is a number in a
  workflow file rather than an assumption in the client.
- **`--patch-from` is not a compression knob and does not behave like one**, and
  the single most expensive setting is the one nobody would look at. Sweeping the
  store archive:

  | Setting | Patch |
  |---|---|
  | `-9`, threads as the tool chooses | 127.8 KiB |
  | `-17`, threads as the tool chooses | 323.5 KiB |
  | `-19`, threads as the tool chooses | 178.9 KiB |
  | `-19 --single-thread` | **84.3 KiB** |

  Multithreaded compression cuts the input into jobs that are compressed
  independently, and a patch is made of nothing but long-range matches — so the
  threads that make compression faster are the threads that throw away the
  matches. Two point one times larger at level 19, three point eight at 17, and
  non-monotonic in the level, which is how a knob behaves when it is the wrong
  knob. The generator therefore sets `ZSTD_c_nbWorkers = 0` explicitly rather
  than leaving it to the default, with this measurement in the comment, and the
  level is pinned at 19 like everything else week 10 pinned.
