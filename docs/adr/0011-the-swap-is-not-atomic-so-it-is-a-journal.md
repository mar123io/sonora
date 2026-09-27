# ADR 0011 — The swap is not atomic, so it is a journal

- **Status:** accepted
- **Date:** 2026-11-30

## Context

The roadmap calls step 5 of the updater "atomic swap at application exit", and
that phrase is doing a lot of work. A single `MoveFileEx` *is* atomic — the
directory entry either moves or it does not. Replacing an installation is not one
rename. It is at least three operations:

```
rename  Sonora      -> Sonora.old
rename  Sonora.new  -> Sonora
delete  Sonora.old                  (only once the new version has started)
```

Between the first and the second, **there is no installed copy of Sonora at all.**
The window is microseconds wide and the machine does not care: a power cut, a
forced reboot, a killed process, a laptop lid on a dying battery. A user who
finds an empty directory where their music player used to be has not experienced
a rare event, they have experienced the one failure that an updater exists to
prevent.

And the third operation cannot happen yet, because of the other half of this
week's requirement: the previous version has to stay on disk until the new one has
proved it can start. Rollback and crash-safety are not two features. They are the
same state — *an update in flight* — asked about at two different moments.

So what is needed is not a swap. It is a small number of steps, each of which is
idempotent, with a durable record of which one was intended, so that a process
starting up can finish or undo whatever the last one left behind. That is a
journal, and it is the same idea as the write-ahead log in the SQLite file two
directories away.

One more constraint shapes everything. The code that moves these directories
cannot live inside them. Windows will let you rename a running executable, but it
will not let you delete one, and a rollback has to remove a tree that the running
process was loaded from.

## Decision

**An update is a sequence of single steps chosen by a pure function of the journal
and the filesystem, and every directory move is performed by a copy of the
updater running outside the tree it is moving.**

### The state

```
<root>/Sonora            the installation
<root>/Sonora.new        the staged tree: unpacked, hashed, complete
<root>/Sonora.old        the previous tree, kept until the new one has started
<root>/Sonora.journal    the only durable state the updater has
```

All four are siblings, which is not tidiness: a rename between two directories on
the same volume moves a directory entry, and a rename across volumes copies
gigabytes. `%LOCALAPPDATA%` holds all of them.

The journal records a stage — `idle`, `staged`, `moving-out`, `moving-in`,
`unconfirmed` — the version being moved away from, the version being moved to, how
many times the new one has been given a chance to start, and the list of versions
that failed to start and will not be offered again. It is written the way the
durable store writes: to a temporary file, flushed, then renamed over the old one,
so a crash during the write leaves the previous journal rather than half of a new
one.

### The decision is a pure function

`sonora::update::NextStep(journal, facts)` takes the journal and four facts about
the filesystem — does each of the three directories exist, and what version does
`Sonora/.launch-ok` name — and returns one step and the journal to write before
performing it. The driver is a loop: write the journal, perform the step, look
again. Nothing else decides anything.

This is what makes the hard part testable without Windows, without a filesystem
and without a crash. The test harness is a struct with three directory slots; the
crash injection is a counter that stops the loop after the *n*th operation, for
every *n*; and the assertion after re-running recovery from that state is the
invariant this ADR exists to defend:

> **At every point at which the machine may stop, there is exactly one complete
> installation reachable, and recovery reaches it.**

Not "usually", and not "unless it stops in the wrong place". Every *n*, including
the ones in the middle of a rollback that is itself recovering from a failed
update.

### Launch is a milestone, not a timer

The roadmap says the new version must write a success flag "within 20 s". It will
not, and the reason is worth stating: 20 seconds measures the machine, not the
program. On a cold laptop with a virus scanner reading 140 MiB of CEF for the
first time, a perfectly good version misses the deadline and gets rolled back —
and a rollback caused by slowness is a bug that only appears on the hardware
least able to tolerate it.

So there is no timer. The new version writes `.launch-ok` at the moment it
reaches the state that proves the update worked: the window is up and the UI has
reported that it loaded — an event that has existed since week 2. If it never gets
there, the flag is never written, and the *next* start is what notices. The
deadline is not a duration; it is "before the process ends".

The journal therefore counts attempts. On start, the updater's decision is:

- flag present and it names the staged version → **confirmed**; delete the old tree.
- flag absent and no attempt has been recorded → record one and let the program run.
- flag absent and an attempt was already recorded → **roll back**, and add the
  version to the refused list so the next check does not download it again.

One attempt, not three. A version that cannot start once might be a coincidence,
and preferring the previous version anyway is the cheap mistake: rolling back
costs two renames and a restart, the manifest will offer the same version again,
and the user keeps a program that works in the meantime. Waiting to be sure costs
the user a broken application. When the two errors are that lopsided, take the
early one.

`.launch-ok` can only certify that the program started. A version that launches
and then fails to play anything is confirmed by this mechanism and is wrong, and
no launch flag anywhere can tell the difference. That is what week 12's crash
reporting is for, and it is a different question wearing similar clothes.

### Who runs the code

`sonora-updater.exe` never performs a move from inside the tree. Before doing
anything, it copies itself to `%TEMP%` and re-executes; that copy waits for the
application's process to exit, performs the steps, and — for a rollback — starts
the restored version again.

`Sonora.exe` at startup reads the journal, runs `NextStep` until it is asked to do
something that moves a directory, and if that happens it launches the out-of-tree
updater and exits immediately. It never waits and it never moves anything itself.
Both directions of travel — forward into the new version, backward out of it — go
through one rule.

## Consequences

- **A confirmed update leaves the disk as it found it; an unconfirmed one leaves a
  second copy of the payload.** 213 MiB extra between the swap and the first
  successful start, which is normally the length of one application launch, and
  indefinitely if the user never opens the program again. That is the correct
  bias: the cost of the extra copy is disk, the cost of not having it is an
  application that cannot be recovered without a reinstall.
- **If the new version's updater is itself broken, nothing rolls back.** The
  rollback decision is made by `Sonora.exe`, which is the binary whose ability to
  start is in question. If it cannot reach its first ten lines, the journal is
  never read and the machine is stuck on a version that does not run. This is the
  residual risk of every in-place updater that does not ship a permanently resident
  service, and the honest mitigation is the one already in place: the MSI is still
  there, and reinstalling it is a supported and documented recovery. Writing it
  down beats discovering it.
- **The journal is a schema, and it will be read by versions that did not write
  it.** It carries a version number, and an unrecognised one means the updater does
  nothing at all rather than guessing — which leaves the installation alone, the
  only safe default for a component whose mistakes are measured in whole
  applications.
- **The refused list can strand an installation.** A version that genuinely cannot
  start on this machine is added and never retried, so the user sits on the old
  one until a later version supersedes it. That is the intended behaviour and it is
  also a silence: nothing tells them why they stopped receiving updates. The
  refused list is therefore surfaced in the application's about panel, because a
  refusal the user cannot see is indistinguishable from an updater that is broken.
- **`NextStep` is reachable from the tests and from nothing else that matters.**
  The platform layer gets `MoveDirectory`, `RemoveDirectory`, `DirectoryExists`,
  `ReadTextFile`, `WriteTextFileDurably` and `SpawnDetached`, and ADR 0002's rule
  holds again: the Windows file is six functions long and holds no policy, and the
  macOS and Linux builds compile the decision and skip the six.
- **The step sequence is a state machine, so it can be drawn**, and it is, in
  `docs/updater-states.md`. An updater is the one component where the reader of
  the code a year from now is most likely to be somebody debugging a machine that
  will not start, and a picture is worth more there than anywhere else in this
  repository.
