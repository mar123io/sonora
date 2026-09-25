# ADR 0006 — The audio callback is real-time code, and the rest of the program is not

- **Status:** accepted
- **Date:** 2026-10-19

## Context

Every other thread in this program is allowed to be late. The bridge answers a
query when it answers it; the decode thread reads a file when the disk gets
round to it; the UI repaints when Chromium decides to. Being slow makes those
worse, never wrong.

The device callback is different in kind. The operating system hands it a
buffer and a deadline: at 48 kHz with a 480-frame period, that is ten
milliseconds, every ten milliseconds, forever. Miss it once and the hardware
plays whatever was in the buffer — a click, a repeat, a gap. There is no
recovery, because the moment is gone. And the failure is not proportional to
the mistake: a callback that is usually fine and occasionally waits 30 ms on a
page fault sounds broken, while one that is uniformly slower but never waits
sounds perfect.

What makes this hard to get right is that the dangerous operations do not look
dangerous. They are the ordinary vocabulary of C++:

- **`new`, `delete`, any container that grows.** The allocator takes a lock.
  Some other thread holds it sometimes. `std::vector::push_back` in a callback
  is a lock acquisition with extra steps.
- **Any mutex.** Not because locking is slow — it is fast — but because the
  thread holding it can be preempted by the scheduler, and then the audio
  thread waits for a thread that is not running. Priority inversion is not a
  theoretical hazard; it is the normal outcome under load, which is exactly
  when audio must not break.
- **File and network I/O.** A read that usually comes from the page cache
  occasionally comes from the disk.
- **Logging.** Which is I/O, and usually a lock, and usually an allocation.
- **Exceptions.** Throwing allocates and takes a lock in the unwinder's tables.
- **`std::function`, `std::shared_ptr` copies, `std::string`.** Each can
  allocate, and none of them says so at the call site.

Half of these are invisible in a code review, because the line that does them
looks like every other line in the codebase.

## Decision

**The device callback, and everything it calls, obeys one rule: no allocation,
no locks, no I/O, no logging, no exceptions.**

The rule is written at the top of `src/audio/include/sonora/audio/engine.h` and
`src/platform/iface/include/sonora/platform/audio_device.h`, next to the
functions it governs, because a rule in a document nobody opens is not a rule.

Three consequences follow, and they shape the design of `src/audio/`:

**Everything the callback needs is allocated before it starts.**
`AudioEngine`'s scratch buffer and `RingBuffer`'s storage are sized in the
constructor and never resized. `Render()` calls `memcpy`, `memset` and
floating-point arithmetic, and nothing else.

**The seam between the two worlds is a lock-free ring buffer.** The decode
thread may block on a file for as long as it likes; the callback reads whatever
is there and fills the rest with silence. A mutex around a shared buffer would
have been simpler to write and would have imported the decode thread's latency
into the audio thread, which is precisely what the design exists to prevent.

**The decode thread polls rather than waiting to be woken.** A condition
variable notified by the callback would be tidier, and `notify_one()` takes the
condition variable's mutex. That is the callback taking a lock, so it is out.
The cost is a wake-up every few milliseconds on one thread; the ring holds half
a second, so the poll interval only needs to be short relative to that.

**This is the opposite of the policy in ADR 0004, on purpose.** The bridge
signals failure by throwing, because on the UI thread an error that cannot be
ignored is worth more than the cost of unwinding. Applying that here would be a
bug. Applying this rule to the bridge would mean error codes nobody checks.
Neither policy is "the project's style": each belongs to the thread it is
written for, and the reason the two are stated separately is so that nobody
tidies them into one.

## Consequences

- **`Render()` cannot report a problem.** It counts. `underruns` and
  `frames_missing` are relaxed atomic increments — the cheapest thing that
  crosses a thread boundary — and something outside the callback reads them
  later. An underrun that nobody can measure is indistinguishable from a driver
  glitch, and being able to tell those apart is worth a counter.
- **An underrun is counted per callback, not per frame.** One long gap and a
  hundred short ones are different faults with different causes.
- **The end of a track is not an underrun.** The engine checks whether the
  source is exhausted before counting one, because a metric that reads 1 after
  every successful playback is a metric nobody reads.
- **The rule is testable, and is tested.** `AudioEngine` separates
  `DecodeStep()` from `Render()` so a test can call them in any order — an
  underrun is produced by calling `Render()` without decoding first, not by
  hoping a CI runner is loaded. Every audio test is deterministic and runs in
  microseconds on all three platforms.
- **The rule is not checked by the compiler.** Nothing stops a future change
  from calling `std::to_string` inside `Render()`. The defences are that the
  function is short, that the rule is written where it is broken, and that the
  underrun counter makes the symptom visible. A sanitizer that traps allocation
  on a named thread would be better, and is worth adding when week 12 builds
  the performance gates.
- **`AudioRenderFn` is a raw function pointer and a `void*`,** not a
  `std::function`. It is uglier at the call site and it cannot allocate.
