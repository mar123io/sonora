# ADR 0007 — The library index is a cache of the files, not a database of the music

- **Status:** accepted
- **Date:** 2026-11-02

## Context

Week 7 gives Sonora a library: point it at a folder, get a searchable list of
tracks. The obvious way to build that is a schema — `artists`, `albums`,
`tracks`, foreign keys between them — and to treat the database as the place the
music lives. Every music player that has ever lost somebody's collection to a
corrupted index was built that way.

The alternative is to decide, once, that the files are the truth and the database
is a derived view of them. Nothing exists in the index that was not read out of a
file, which means the index can be deleted at any moment and rebuilt by walking
the folder again.

The two designs are not distinguishable from the outside on day one. They differ
the first time something goes wrong: the schema changes, the file is corrupted,
two versions of Sonora open it, a scan is interrupted halfway. In the first
design each of those is a support problem. In the second, each of them is a
rescan.

There is a real cost to choosing the cache. Anything the user creates that is
*not* in the files — a playlist, a rating, a play count — cannot live in a store
that may be deleted and rebuilt. So that store has to be a different one, with a
different policy, and the boundary between them has to be drawn deliberately
rather than discovered later.

## Decision

**The index is a cache. The files are the source of truth, and everything in
`src/library/` is written so that deleting the database file is a recoverable
event rather than a data loss.**

Five things follow from that, and each one is in the code:

**A path is a track's identity.** `tracks.path` is `UNIQUE` and every write is an
upsert on it. A file that moved is a new track and the old row is removed by the
next scan — Sonora does not try to recognise the same music in a new place. That
is a real limitation: moving an album loses nothing, because there is nothing in
the row that was not read from the file, but it would lose a rating if ratings
lived here. They will not live here.

**There is no `albums` table and no `artists` table.** An album is a value that
several tracks share. The moment it becomes a row, something has to keep that row
in step with the tracks — on every retag, every delete, every scan — and that
synchronisation is the part that goes wrong. `ListAlbums()` is a `GROUP BY` over
an index, which is microseconds on a personal library and is always exactly
consistent with the tracks, because it *is* the tracks.

**The scan is incremental, by modification time and size.** A rescan of an
unchanged folder reads no tags at all: it walks the tree, compares each file
against the stamp the index holds, and stops. This is what makes the cache cheap
enough to verify at every startup, which in turn is what makes it safe to treat
as disposable. It is wrong for a file edited within the filesystem's timestamp
resolution and restored to exactly the same length, and right for everything
anyone actually does to a music file.

**A scan is resumable, not transactional.** Tracks are written in batches, so an
interrupted or cancelled scan leaves the index partly updated — and that is
fine, because the next scan continues from there. The one thing a cancelled scan
must not do is *remove* rows: a walk that did not finish cannot tell a missing
file from an unvisited one, and acting on that would empty the library.

**The full-text index is derived from the table, not stored beside it.** FTS5 in
external-content mode keeps only the inverted index; the text lives once, in
`tracks`, and three triggers keep the two in step. A contentless or standalone
FTS table would mean every title stored twice, with the copies to reconcile.

## Consequences

- **`PRAGMA user_version` is the schema version, and a newer one is refused.** A
  database written by a later Sonora is not opened — guessing what its columns
  mean is how an index gets silently corrupted. The user is told, and their
  options are to update or to delete a cache.
- **A migration may legitimately be "drop everything and rescan".** For a store
  holding user data that would be unthinkable. Here it is a valid, honest
  implementation of a schema change, and it will be used when one arrives that is
  not worth the code to convert.
- **`synchronous = NORMAL`, not `FULL`.** Losing the last batch of a scan to a
  power cut costs a rescan of a few files. Paying an fsync per batch to avoid
  that would be protecting a cache from an outcome it is designed to survive.
- **Playlists, ratings and play counts need a different store**, with real
  durability and real migrations, and they are not in `src/library/`. Week 9
  draws that boundary; this ADR is what it will be drawn against.
- **Tags are not trusted input.** Numbers out of a tag are clamped, text is taken
  as UTF-8 and nothing else, and a file with no title at all is indexed under its
  file name rather than as an empty string. A music folder is full of files
  written by programs that no longer exist.
- **The index is not the playback path.** Nothing in `src/audio/` links
  `sonora::library`, and the player still opens a file by path. The library
  decides *what* to play; it has no opinion on how, which keeps the real-time
  half of the program free of a database.
- **One connection behind one mutex.** Not because SQLite could not do better,
  but because the contention in a personal library is a scan's write batch
  against a list query. When that stops being true, the answer is a connection per
  thread with WAL — not a finer lock inside `Library`.
