# ADR 0005 — Events are pushed by the shell and coalesced before they arrive

- **Status:** accepted
- **Date:** 2026-10-12

## Context

Everything on the bridge so far goes one way: the page asks, the shell answers.
That is enough for versions and capabilities and nothing else. The features this
project exists to build are all the other direction — playback position moves,
a buffer fills, a download progresses, the operating system's media keys are
pressed, an update finishes staging. In every one of those the shell knows
first and the page has to be told.

Two problems come with that, and they are usually solved separately and badly.

**How does the shell speak first?** CEF's message router already carries
queries, and it supports *persistent* queries: one request from the page,
many answers from the native side. It looks like the obvious answer.

**How often does it speak?** The producer and the consumer of an event run at
rates that have nothing to do with each other. An audio callback fires every few
milliseconds; a human reads a number four times a second at most. One message
per production means the renderer spends its time parsing JSON about a progress
bar instead of painting one, and it gets worse exactly when the machine is
already busy.

## Decision

**Events are pushed, not subscribed to through a query.** The shell calls one
function on the page — `window.__sonoraEvents` — with a batch. Persistent
queries stay refused, with a message that says why.

A persistent query looked right and is not, for three reasons. It ties every
event to a call the page happened to make, so a reload silently ends the stream
and nothing says so. It ties the stream to the frame that opened it, so the
lifetime of an event source is the lifetime of a DOM object that knows nothing
about it. And it cannot answer the case that matters most: the shell needs to
be able to speak before the page has asked anything at all — about an update
that is ready, or a device that disappeared.

**Events are declared in the same schema as methods.** `tools/gen_bridge.py`
emits a payload struct per event and a `bridge::Events` class with one method
per event, so the string `"diagnostics.heartbeat"` is written exactly once, in
the schema. An event that does not exist is a compile error on the native side
and a type error in the page, which is the same property ADR 0004 bought for
methods and for the same reason.

Whether an event coalesces is a property of the event, set in the schema, not
an argument passed at the call site. Two call sites that disagree about it is a
bug nobody would ever find.

**Rate limiting lives in `src/bridge`, not in the shell.** `EventCoalescer`
takes a minimum interval, a clock and a "call me back in N milliseconds"
function, and has no idea CEF exists. It holds at most one pending event per
name for coalescing events — latest wins — and queues the others. What is left
in `src/shell/cef/event_channel.cpp` is a UI-thread timer and one call into the
page per batch.

This is the same split as `src/assets/` in week 2 and `src/bridge/` in week 3,
and it has the same payoff: the logic that decides *what the page sees* is
tested on three platforms with an injected clock, in microseconds, instead of
being tested on Windows by watching a number on a screen.

## Consequences

- **The first event is not delayed.** The interval bounds a stream; it is not a
  reason to make the page wait a quarter of a second for the first thing it ever
  hears. The coalescer starts one interval in the past.
- **A quiet spell does not cost an interval.** An empty flush deliberately does
  not move the "last flushed" mark, or an event arriving after a minute of
  silence would wait for a window that has long since passed.
- **Gaps in the sequence are the feature.** The heartbeat carries a counter that
  increments on every emission, including those that never arrive, so the page
  can display 20 Hz in and 4 Hz out. An event stream where nothing was ever
  dropped would mean the coalescer is not doing anything.
- **Events with nowhere to go are dropped where they are made.** Before the
  browser exists and after it has closed, `EventChannel::Emit` returns without
  queueing. A page whose first frame arrived with a backlog describing a past it
  never saw would be worse than one that missed it.
- **The queue is bounded.** 256 pending events, oldest dropped, counted. Reaching
  that means the host has stopped flushing, which is a bug — but an unbounded
  queue turns that bug into an out-of-memory instead of a counter going up.
- **The coalescer is single-threaded on purpose.** No mutex: the shell posts
  from the CEF UI thread and CEF's own task posting is what gets other threads
  onto it. Week 5's audio callback must not block on a lock held by whatever is
  serialising JSON, so the hand-off from that thread will be explicit and
  visible rather than hidden inside this class.
- **The payload crosses as a JSON string, not as JavaScript.** The shell builds
  `__sonoraEvents("<json>")` and the page parses it. Nothing from the native side
  is ever evaluated as source. This relies on JSON being a subset of JavaScript
  string syntax, true since ES2019, which CEF's pinned Chromium is far past.
- **A newer shell can send events this bundle has never heard of.** The page
  logs that once, at debug level, and carries on. That is the protocol working,
  not failing.
