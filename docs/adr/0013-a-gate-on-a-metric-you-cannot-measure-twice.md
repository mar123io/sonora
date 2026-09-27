# ADR 0013 — A gate on a metric you cannot measure twice is a coin toss

- **Status:** accepted
- **Date:** 2026-12-07

## Context

The roadmap for this week asks for one thing in one line:

> Job CI che fallisce se una metrica peggiora oltre il 10% rispetto alla baseline salvata.

That is a reasonable sentence and it is most of a bad design, and the reason is a number I
measured before writing the job. Six invocations of the same benchmark, on the same machine,
against the same code, with nothing else changed:

```
library-scan-rescan, 20,000 files:  93.7  86.1  85.8  85.1  80.2  87.2  ms
```

A range of 8% either side of the middle — against a 10% gate. And every one of those
invocations, asked how noisy it was, answered about 2%: the spread *inside* one process is
not the spread *between* processes, and a benchmark cannot see the second one by looking at
itself once.

So the naive version of the roadmap's job fails builds at random, and the failures are
indistinguishable from real ones. That is worse than no gate, because a gate nobody trusts
gets switched off in a hurry by somebody who needs to ship, and then there is no gate and no
record of when it went.

## Decision

**A metric is gated only when the measurement knows its own median well enough to support
the threshold. The harness works out whether that is true, from the same samples, and says
so when it is not.**

Three parts, and each one is a mistake avoided rather than a feature added.

### The gate consults the uncertainty of the median, not the spread of the samples

What a comparison compares is two medians. The uncertainty of a median falls as the square
root of the sample count; the spread of the samples does not fall at all. A gate built on
the spread would refuse a metric no matter how carefully it was measured, and would therefore
teach everybody to stop measuring carefully.

```
uncertainty ≈ 1.858 × (MAD / median) / √n
```

— the standard error of a median, `1.2533·σ/√n`, with `σ` estimated as `1.4826·MAD`. Both
constants are the ordinary normal-distribution ones and both are approximations. What matters
is that the number shrinks with `n`, that it is the same formula on both sides of the
comparison, and that **measuring longer buys a tighter gate**, which is the incentive worth
building in.

MAD rather than a standard deviation, for the reason every benchmark harness eventually
learns: one run that took four times as long because something else on the machine woke up
should not decide whether the metric can be judged, and a mean-based spread lets it.

### A metric whose medians are known to more than half the threshold is reported, not judged

`noise = baseline.uncertainty + current.uncertainty`, and if that exceeds
`threshold × 0.5` the verdict is `too noisy to gate`: the change is printed, with its size,
and the build is not failed. Half rather than all of the threshold, because a gate that fires
exactly when the noise reaches the threshold fires half the time on unchanged code.

This is the part that makes the whole thing usable, because it is per metric rather than per
job. Measured on the machine this was written on, with a 10% threshold and a baseline
recorded over 27 samples:

| Metric | median | spread | uncertainty | gated at 10%? |
|---|---|---|---|---|
| `update-delta-size` | 525,157 bytes | 0.00 % | 0.00 % | yes — and could be gated at 1% |
| `library-scan-cold` | 251.7 ms | 4.28 % | 1.53 % | yes |
| `library-scan-rescan` | 13.46 ms | 5.36 % | 1.92 % | yes |
| `update-patch-apply` | 1.095 ms | 5.06 % | 1.81 % | yes, barely |

The contrast at the top and bottom of that table is the argument. The size of a delta is the
same number every single run, so it can be gated as tightly as anybody likes; a patch that
applies in one millisecond is mostly measuring the clock, and with fewer samples it drops out
of the gate by itself rather than by somebody's judgement.

### A baseline records samples from several runs, not the summary of one

This is the fix for the number at the top of this document. A baseline built from one
invocation claims to know its median to within a fraction of a percent, because inside one
process the measurement really is that steady. Appending three independent runs into the same
baseline — `sonora_bench --baseline … --append`, three times, which is what the recording job
does — makes it admit to the 4–5% it actually has.

Without that, the gate would be tight, confident and wrong. With it, the `library-scan-rescan`
metric reports a 5.36% spread over 27 samples, an uncertainty of 1.92%, and gates at 10% with
room to spare.

The samples are therefore in the baseline file, capped at 64 per metric with the oldest
dropped first: a baseline appended to for a year should describe the machine running the build
and not the machine of two years ago.

## Consequences

- **The baseline belongs to a machine, and that machine is the runner.** A baseline recorded
  on a developer's laptop and compared against on a CI runner measures the difference between
  two computers. The recording job runs on the same image as the gate, three times, and a
  person commits the result — which is also what makes a deliberate baseline change visible in
  a diff.
- **The first push has no baseline and gates nothing.** Every metric comes out as `no
  baseline`, the job passes, and it uploads what it measured so that it can become one. A new
  benchmark arrives the same way, which is how one can be added without breaking the build
  that adds it.
- **Deleting a benchmark fails the build.** The easiest way to make a performance gate green
  is to stop measuring, so a metric in the baseline that was not measured is a failure and
  removing one on purpose costs a visible commit to the baseline. The same applies to a metric
  whose unit changed: 1000 ms and 1 s are the same duration, and comparing them without
  noticing reports a thousandfold improvement.
- **Three of the four metrics are things a user never waits for.** The roadmap also asked for
  time to first frame, resident memory at rest and CPU over five minutes of playback, and
  those need the application, a window and a sound card. They are not approximated here: a
  benchmark that cannot run in CI stops being true without anybody noticing, so they are named
  in the week's "to come back to" and measured by nothing. What is here runs identically on a
  runner and on a laptop, which is the only way a number committed to a repository still
  means something a month later.
- **The threshold is one number for every metric, and it should not be.** `update-delta-size`
  could be gated at 1% and is gated at 10% along with everything else, which means a 5% growth
  in every delta would pass unnoticed. Per-metric thresholds are a field in the baseline and
  half an hour's work, and they are not done: the value of the uniform number today is that
  there is nowhere to hide a metric by quietly loosening its own gate.
- **A 10% gate cannot catch a 10% regression.** The optimisation this week found was 13%, and
  it would have been caught; an 8% one would not. That is the price of a threshold that does
  not fire on noise, and the honest way to lower it is more samples rather than a smaller
  number.
