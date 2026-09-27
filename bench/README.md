# Benchmarks, and the gate that reads them

`sonora_bench` measures four things and compares them against a recorded baseline. A
metric that got more than 10% worse fails the build; a metric whose measurement cannot
support a gate that tight is reported and gated on nothing.

That second rule is the whole design, and it is
[ADR 0013](../docs/adr/0013-a-gate-on-a-metric-you-cannot-measure-twice.md). The short
version: the same code, unchanged, measured 80.2 ms to 93.7 ms across six runs of the
suite, while each individual run reported a spread of about 2%. A gate built on one run's
opinion of its own noise would have failed builds at random, and a gate that fails at
random is turned off within a fortnight.

## What is measured

| metric | unit | what it is |
| --- | --- | --- |
| `library-scan-cold` | ms | indexing 4,000 files into an empty index |
| `library-scan-rescan` | ms | the same folder, already indexed and unchanged |
| `update-delta-size` | bytes | the patch between two builds that differ by one file |
| `update-patch-apply` | ms | reconstructing the new package from the old one plus the patch |

Four, and not the seven the roadmap named. Time to first frame, RSS at rest and CPU over
five minutes of playback all need a window, a GPU and a sound card; a CI runner has none of
those, and a number measured on a machine that has none of them is worse than no number.
They are named in ADR 0013 as measured by nothing, which is at least true.

`update-delta-size` has a spread of exactly zero, because it is not a timing: the same two
inputs produce the same patch, byte for byte. It is in here precisely because of that — it
is the one metric a 1% gate would be reasonable on, and it is the check that the reason the
others cannot be gated that tightly is the measurement and not the rule.

## Running it

```
cmake --build --preset linux-release        # or win-release, or mac-release
./build/linux-release/bin/sonora_bench --repetitions 9
```

It builds its own test data — 4,000 tiny valid WAV files and two package trees — in a
working directory it creates and reuses. Nothing it does touches a real library.

Useful flags:

```
--repetitions N     how many times each metric is measured (at least 3; 9 in CI)
--files N           how large the synthetic library is (default 4,000)
--workdir DIR       where the test data goes
--baseline FILE     compare against this file, and fail on a regression
--out FILE          write this run's numbers, whatever the verdict
--only NAME         one metric, for when you are iterating on it
--threshold F       the gate, as a fraction (default 0.10)
--noise-budget F    how much of the threshold the noise may be (default 0.5)
--update-baseline   replace the baseline with this run
--append            add this run's samples to the baseline instead of replacing it
```

## The baseline

`baseline-linux-x64.json` is recorded **on the CI runner**, not on a laptop, and this is
not a formality: the numbers are a property of a machine. A GitHub-hosted runner indexes
those 4,000 files in a few hundred milliseconds, a developer's desktop in rather less, and
comparing one against the other produces a 40% "regression" on the first commit of the
week.

So there is one baseline, it belongs to `ubuntu-latest`, and it is committed to this
repository so that a pull request is measured against the same numbers whoever opens it.

### Recording it

1. Actions → **ci** → Run workflow → tick **record_baseline**.
   The job runs the suite three times and appends the samples into one file; the MSI job
   sits it out.
2. Download the `sonora-bench` artifact and commit `baseline-linux-x64.json` into this
   directory.

Three runs rather than one because the spread inside a single process is the spread of that
process's luck — the point ADR 0013 is about. The file keeps every sample, not just the
median, which is what lets a later run be appended rather than thrown away: measuring for
longer is how a gate becomes tighter, and that is the incentive it should have.

### When a change moves a number on purpose

A real improvement fails nothing — it is reported as `improved` and the gate stays green.
But the baseline is now pessimistic, and the *next* regression will be measured against a
number nobody is achieving any more. So a change that moves a metric deliberately carries
its own baseline:

```
./build/linux-release/bin/sonora_bench --baseline bench/baseline-linux-x64.json --update-baseline
```

...and the new file goes in **the same commit** as the change that moved it, with the
reason in the commit message. A baseline updated in a commit of its own is a baseline
nobody can explain six months later.

The honest caveat: run locally, that command writes a laptop's numbers into a file the
runner will be judged against. Do it through the recording workflow above instead, and use
the local form only when `--only` is limiting it to a metric that is not a timing.

## Proving the gate works

The gate is checked by making it fail on purpose:

```
git switch -c slow-on-purpose
# in src/library/src/scanner.cpp, make HasAnyExtension build a lowercased
# std::string per candidate again -- the allocation the week-12 change removed
git commit -am "deliberately slow, to see the gate fail"
gh pr create --fill
```

The `linux-x64 (release, benchmarks)` job goes red with `library-scan-cold ... REGRESSED`,
and the pull request cannot be merged. Then throw the branch away.

It is worth doing once, on purpose, because a gate nobody has seen fail is a gate nobody
knows is wired up — and this one spent its first afternoon reporting `too noisy` for
everything, which looks identical to working.
