# ADR 0008 — What the user did lives in a different store from what their files say

- **Status:** accepted
- **Date:** 2026-11-09

## Context

[ADR 0007](0007-the-library-index-is-a-cache.md) decided that the library index
is a cache of the files: it may be deleted at any moment, and everything in it
can be rebuilt by walking the folder again. That ADR also wrote down the bill
that decision runs up, and left it for this week:

> Playlists, ratings and play counts need a different store, with real
> durability and real migrations, and they are not in `src/library/`. Week 9
> draws that boundary; this ADR is what it will be drawn against.

Week 9 needs the first of those. A jump list shows the albums somebody played
recently, and "recently played" is not a fact about any file. No amount of
rescanning produces it. It is the first datum in the program that exists only
because a person did something.

The tempting move is one `ALTER TABLE`. The index is right there, it already
has a row per track, and `last_played_at INTEGER NOT NULL DEFAULT 0` is one
line. It would work, and it would quietly repeal ADR 0007: from that line
onwards, deleting the index is no longer a recoverable event, and every
consequence that followed from "this file is disposable" — `synchronous =
NORMAL`, a migration that may legitimately drop everything, a schema version
that tells the user to delete a cache — would be false without anything in the
code saying so.

There is a second reason, and it is the one that turns a principle into a bug.
The index's ids are cache-local. Delete the index, rescan, and every track has a
different number. Week 9 registers jump-list entries with Windows, and those
entries are URLs that this process is handed back later — after a restart, after
a rescan, after an update, possibly weeks later. A URL carrying an index id
would mean one song this month and a different one next month, and the failure
is silent: the right kind of thing plays, just not the thing that was clicked.

## Decision

**There are two stores, with opposite policies, and the boundary between them is
the file path.**

| | `sonora::library` (`library.sqlite`) | `sonora::state` (`state.sqlite`) |
|---|---|---|
| Holds | what the files say | what the person did |
| Rebuilt by | rescanning | nothing |
| May be deleted | yes, costs a rescan | no, costs the data |
| `synchronous` | `NORMAL` | `FULL` |
| Identity | row id, reassigned on rebuild | path, plus a durable id that is never reused |
| A newer schema | refused, and deleting it is a valid answer | refused, and there is no other answer |

Three rules make the boundary real rather than decorative:

**The durable store keys by path.** That is the identity ADR 0007 already chose
for a track, and it is the only name the two stores can share. The state store
never holds an index id, and the index never holds a durable one.

**The durable store issues the ids that leave the process.** `sonora://track/<id>`
and `sonora://album/<id>` carry a durable id — a row of `state.files`, created
the first time something needs a stable name for a file, `AUTOINCREMENT` so that
a number is never reused after a delete. Resolving a link is two steps: durable
id → path → index row. The extra step is the point.

**A URL never names a file.** Week 7 removed filesystem paths from the bridge by
addressing tracks with ids; a registered URL scheme is the same boundary, facing
the other way, and arriving from anyone at all. `ParseDeepLink` accepts a run of
decimal digits and nothing else — no percent-decoding, no second path segment,
no id that does not fit in an `int64` — and what comes out still has to exist in
the durable store and then in the index before it means anything.

## Consequences

- **Two SQLite files in the user data directory**, and two sets of the same
  helper code. `Statement`, `Execute` and `Transaction` are written twice, on
  purpose: what they would have to share is the exception type, and that is the
  one thing the two stores must not share — the shell catches `LibraryError` to
  turn a failed query into something the page can show, and a failure in the
  durable store is not that. Ninety lines of visible duplication beats a common
  base class that says something untrue about both.
- **Deleting `library.sqlite` is still free.** The jump list survives it, because
  it was never built out of the index in the first place. This is the property
  the `ALTER TABLE` would have destroyed, and it is checkable: delete the index,
  restart, and the recent albums are still there once the rescan has finished.
- **The durable store can be wrong about the world.** It remembers paths, and
  paths move. A file that is no longer in the index may have been deleted, or may
  be on a drive that was not plugged in this morning — and those two look
  identical from here. So nothing forgets anything automatically: `Forget()`
  exists, it takes an explicit list, and the shell decides when (today: never).
  Losing somebody's history because they booted without their external disk is
  exactly the failure this store was created to prevent.
- **This is where playlists and ratings will go**, with the same policy and the
  same migration discipline. The table in it today has four columns; what matters
  is that the second store now exists, has a schema version of its own, and has
  somewhere to grow that is not the cache.
- **The library index stays at schema 2.** Week 9 adds no column to it, which is
  the observable form of this decision.
