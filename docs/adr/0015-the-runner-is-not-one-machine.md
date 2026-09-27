# ADR 0015 — The runner is not one machine, so a duration is not a baseline

- **Status:** accepted
- **Date:** 2026-12-14
- **Amends:** [ADR 0013](0013-a-gate-on-a-metric-you-cannot-measure-twice.md)

## Context

ADR 0013 built a performance gate that refuses to judge a metric it cannot measure twice, and
it ends with a consequence stated as settled:

> **The baseline belongs to a machine, and that machine is the runner.** A baseline recorded
> on a developer's laptop and compared against on a CI runner measures the difference between
> two computers.

The sentence is right and the last four words of the heading are wrong. `ubuntu-latest` is not
a machine. It is a fleet of machines that agree about the operating system and about nothing
else, and the gate was comparing a median measured on one of them against a median measured on
another.

The push of the `v1.0.0` tag is what said so, and it said it about as clearly as a measurement
can. Between the commit that recorded the baseline and the tag there are six commits: a
signing key, a test key, a PowerShell quoting fix, a change to a CEF capability in a file the
Linux job does not compile, and two documentation commits. Nothing in the scanner. Nothing in
the update code. Nothing, in fact, in any translation unit that the benchmark links.

```
library-scan-cold    133.467 -> 275.660 ms      +106.5% +/- 0.8%   REGRESSED
library-scan-rescan   19.755 ->  26.198 ms       +32.6% +/- 0.2%   REGRESSED
update-delta-size    525158.000 -> 525158.000 bytes  +0.0%         unchanged
update-patch-apply     0.208 ->   0.499 ms      +139.4% +/- 4.9%   REGRESSED
```

Four things in that table, and they have to be read together.

**The suite contained its own control group, by accident.** `update-delta-size` is the one
metric that is a count and not a duration, and it came back identical to the byte — 525,158
both times. The delta is taken over the same two archives by the same code, so a bit-identical
result is proof that the code under measurement did not change. Everything that was timed got
worse; the one thing that was counted did not move at all. No change to this repository can
produce that pattern.

**The three factors are different from each other**: ×2.07 on the cold scan, ×1.33 on the
rescan, ×2.40 on the patch. So this is not one machine with a slower clock. The cold scan is
dominated by the filesystem and by TagLib, the rescan by SQLite and `stat`, the patch by memory
bandwidth, and those three resources were provisioned worse by three different amounts. Whatever
host ran the tag had storage roughly half as fast and memory roughly half as fast, and a CPU
only somewhat slower.

**The spreads are tiny**: 1.23% and 0.29%. The slow host was not unstable. It was reliably,
repeatably, boringly slow, and it would have said so however long we measured it. This is the
part ADR 0013's machinery cannot see: `1.858 × (MAD/median)/√n` describes how well we know the
median *on the machine we are standing on*, and no amount of `n` tells us anything about the
next machine.

**And the line that settles it.** The pull request from week 12 deliberately made the scanner
worse — it reads the first 4 KiB of all four thousand files — and it measured **217.877 ms**.
The tag, with that patch nowhere near it, measured **275.660 ms**. The sabotaged code was
twenty per cent faster than the clean code, because it happened to land on a better host.

That is the number that decides the design: **variance between hosts reached +106.5%, and the
regression the gate exists to catch was +55%.** Those two ranges overlap. There is no
threshold, anywhere, that separates them, and there is no number of samples that helps, because
more samples do not make a slow machine less slow — they make us more confident that it is.

There is a second, smaller finding in the same data, and it sharpens the first. Every one of
the three baseline runs had the same shape in its first iterations:

```
0.878  0.639  0.277  0.247  0.208  0.201  0.204  0.211  0.199
0.850  1.086  0.267  0.212  0.195  0.211  0.195  0.194  0.194
0.824  1.018  0.243  0.198  0.194  0.201  0.194  0.193  0.196
```

Twenty lines that allocate, touch and free a buffer of the same 8,523,776 bytes in a loop
reproduce that curve exactly, with no Sonora in them at all: glibc serves the first large
allocation from `mmap`, gives it back with `munmap`, and only after a couple of rounds raises
its own threshold and begins reusing pages the kernel has already faulted in. So six of the
twenty-seven samples in the baseline were measuring the kernel, at up to 1.086 ms against a real
cost of 0.195.

Discarding the first three iterations fixes it, and here is the part worth writing down: doing
so takes the metric's uncertainty from 2.41% to **0.99%**. Cleaning up the measurement makes the
gate **tighter**, and therefore **more likely** to fail a build on a machine it cannot see. The
garbage samples had been acting as an accidental safety margin, and removing them — which is
correct — makes the real problem worse.

## Decision

**A duration fails a build only where a person is about to look at it. A count fails everywhere.
A benchmark that went missing fails everywhere, in every mode.**

Three parts.

### Durations fail on a pull request and are reported elsewhere

`sonora_bench --report-only` prints the whole comparison, writes the numbers, and exits 0 if the
only complaint is `REGRESSED`. CI passes it on every event except `pull_request`.

The reasoning is about what a failure can accomplish. On a pull request a red gate reaches
somebody who is deciding whether to merge, who can read the table, re-run the job, and tell a
regression from a slow host by doing exactly what this document did. On a push to `main` the
decision has been made. On a tag the code has been in `main` for days and the only thing a
failure can do is refuse to build a release — which is what happened, and which is how a project
ends up unable to ship because a metric had a bad afternoon.

### A count is gated everywhere, and could be gated far tighter

`update-delta-size` was identical to the byte across two different hosts three weeks apart. It
is the only metric here that measures the software rather than the software plus a computer, and
it is the one that guards the thing most worth guarding: a change to the compression or the
delta format that quietly multiplies what every installation has to download. It keeps its gate
on every event.

### A metric that disappears fails in every mode

`--report-only` downgrades `kRegressed` and nothing else. A metric in the baseline that was not
measured, or whose unit changed, still fails — because that verdict is a statement about this
repository and not about this host, it reads the same on every machine there is, and it is the
one somebody could use to make the gate green by deleting a benchmark. The distinction has a
name in the code, `IsMachineIndependentFailure`, and a test of its own rather than a branch in the runner
that nobody checks.

## Consequences

- **A regression that reaches `main` without a pull request ships unremarked.** Direct pushes
  to `main` are how that happens, and the honest mitigation is not a gate but a branch rule.
  Written here rather than discovered later.
- **Week 12's demonstration survives.** The deliberate regression was a pull request, which is
  the one event where the gate still fails, and the screenshot of it failing is still a true
  picture of what the gate does. It is also, now, a picture of the gate getting the right answer
  for partly the wrong reason: of the `+63.2%` it reported, about half was the patch and about
  half was the host.
- **The gate is weaker than the roadmap asked for, and the roadmap was asking for something
  that cannot be built this way.** "Fails if a metric gets 10% worse than the saved baseline"
  requires the two measurements to come from the same computer. On hosted CI they do not, and
  the choice is between a gate that fires at random and a gate that reports.
- **Three candidates were considered and not taken.**
  - *One calibrator.* Measure the machine with something that has nothing to do with Sonora — a
    `memcpy` over a fixed buffer — and gate on `metric / calibrator`. Rejected on the numbers
    above: the three metrics scaled by 2.07, 1.33 and 2.40, so one ratio cannot cancel three
    resources that vary independently. This was the first idea and the data killed it.
  - *A baseline recorded across many separate CI runs.* `--append` already does it; CI would
    only have to download the previous artefact. **Prediction, recorded before trying it:** with
    between-host variance inside the samples, every duration's uncertainty exceeds the 5% budget
    and every duration comes out `too noisy to gate`. That is the correct answer to the question
    and a gate that gates nothing, so it is worth running once as a demonstration and not worth
    shipping.
  - *A self-hosted runner.* The actual fix, and not free: a machine somebody owns, keeps
    running, and keeps identical for as long as the baseline is supposed to mean something. A
    project that cannot promise that should not pretend its numbers are comparable.
- **The open question is two calibrators, not one.** One for storage (write, `fsync` and read
  back a fixed number of fixed-size files) and one for memory bandwidth, gating
  `library-scan-* / storage` and `update-patch-apply / memory`. Whether it works is an empirical
  question with a cheap experiment: run the workflow several times on one commit and look at the
  spread of the ratios rather than of the durations. **Prediction, again recorded first:** the
  ratios will be tighter than the durations and not tight enough for a 10% gate, because the
  cold scan is not purely storage — it is storage and TagLib and `std::filesystem` — and a
  calibrator that does not decompose the same way cannot cancel it. If that prediction holds,
  durations in this project stay reported and never gated, and the sentence to put in the README
  is that the project measures four things and gates one of them.
- **The warm-up fix is right and it is not an improvement to the gate.** It goes in because a
  benchmark should measure the thing it names; it makes the gate's noise estimate smaller, which
  under a fleet of unequal machines makes false failures more likely, not less. Two fixes that
  each help and that pull in opposite directions is the ordinary condition of this kind of work
  and it is better written down than rediscovered.
