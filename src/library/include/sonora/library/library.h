#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <sonora/library/track.h>

struct sqlite3;

namespace sonora::library {

class LibraryError : public std::runtime_error {
 public:
  explicit LibraryError(const std::string& message) : std::runtime_error(message) {}
};

// What the incremental scan compares against, for every file the index knows.
struct FileStamp {
  std::int64_t id = 0;
  std::int64_t mtime_ns = 0;
  std::int64_t size_bytes = 0;
};

// The library index: one SQLite file, one table of tracks, one FTS5 index over
// the text in it.
//
// There is no albums table and no artists table. An album is a value that
// several tracks share, and the moment it becomes a row somebody has to keep
// that row in step with the tracks -- on every retag, every delete, every
// scan. SQLite groups fifty thousand rows in single-digit milliseconds, which is
// faster than the frame this list is being drawn into, so the table would buy
// nothing and cost a class of bug that is genuinely hard to find. ListAlbums()
// is a GROUP BY.
//
// The index is a cache, not a source of truth. The files are the truth; this
// can be deleted and rebuilt by walking them again, and the scanner is written
// so that rebuilding is the same code path as updating. That is why nothing in
// here is precious: no user data lives only in this file.
//
// Threading: every public method takes one mutex, and there is one connection
// behind it. A personal library is tens of thousands of rows, the queries are
// indexed, and the caller is either the UI thread asking for a page of a list or
// the scanner writing a batch -- so the contention is a scan's write batch
// against a list query, measured in microseconds. When that stops being true the
// answer is a connection per thread with WAL, not a finer lock in here.
class Library {
 public:
  // Bumped whenever the schema changes; the constructor migrates forward from
  // any older version and refuses to touch a newer one.
  //
  // Version 2 added the covers table, tracks.cover_hash and settings.
  static constexpr int kSchemaVersion = 2;

  // ":memory:" gives a throwaway index, which is what the tests use and what
  // makes this target testable in CI with no filesystem at all.
  explicit Library(const std::filesystem::path& database_path);
  ~Library();

  Library(const Library&) = delete;
  Library& operator=(const Library&) = delete;

  // Inserts by path, or updates the row that path already has, and returns its
  // id. Path is the identity of a track here: a file that moved is a new track
  // and the old row is removed by the scan, which is both simpler and more
  // honest than pretending to recognise the same music in a new place.
  std::int64_t Upsert(const Track& track);

  // One transaction for the lot. The scanner uses this because committing once
  // per file turns a scan of a real music folder into one fsync per file, and
  // that is the difference between seconds and minutes.
  void UpsertBatch(const std::vector<Track>& tracks);

  void Remove(std::int64_t id);
  void RemoveBatch(const std::vector<std::int64_t>& ids);

  [[nodiscard]] std::optional<Track> Get(std::int64_t id) const;
  // Several ids in one query, in the order asked for, skipping the ones that are
  // not there. What player.enqueue is built on: queueing an album must not be
  // twelve round trips through a mutex.
  [[nodiscard]] std::vector<Track> GetMany(const std::vector<std::int64_t>& ids) const;
  [[nodiscard]] std::optional<Track> FindByPath(const std::string& path) const;
  [[nodiscard]] std::int64_t TrackCount() const;
  [[nodiscard]] std::int64_t AlbumCount() const;
  [[nodiscard]] std::int64_t ArtistCount() const;

  // Ordered the way a person expects an album to be ordered: album artist,
  // album, disc, track number. `limit` below zero means everything.
  [[nodiscard]] std::vector<Track> ListTracks(std::int64_t limit = -1,
                                              std::int64_t offset = 0) const;
  [[nodiscard]] std::vector<AlbumSummary> ListAlbums() const;
  [[nodiscard]] std::vector<std::string> ListArtists() const;
  [[nodiscard]] std::vector<Track> AlbumTracks(const std::string& album,
                                               const std::string& album_artist) const;
  [[nodiscard]] std::vector<Track> ArtistTracks(const std::string& artist) const;

  // Full-text search over title, artist, album and album artist, best match
  // first. The last word is treated as a prefix, so the list narrows while
  // somebody is still typing.
  //
  // Whatever is in the search box is text, never syntax: see MakeMatchQuery.
  [[nodiscard]] std::vector<Track> Search(const std::string& text,
                                          std::int64_t limit = 200) const;

  // Everything the scan needs to decide what to do, in one query rather than
  // one query per file. Keyed by path.
  [[nodiscard]] std::unordered_map<std::string, FileStamp> Stamps() const;

  // ---- covers ------------------------------------------------------------
  //
  // Stored by content hash, which is what makes an album's twelve copies of the
  // same JPEG one row. The scan asks HasCover before it hands over any bytes, so
  // a rescan of an album reads its artwork once and stores it never.

  [[nodiscard]] bool HasCover(const std::string& hash) const;
  void PutCover(const std::string& hash, const Cover& cover);
  [[nodiscard]] std::optional<Cover> GetCover(const std::string& hash) const;
  [[nodiscard]] std::int64_t CoverCount() const;

  // Drops covers no track refers to any more, and returns how many went. Called
  // at the end of a scan: a deleted album would otherwise leave its artwork in
  // the file forever, which is how a cache becomes a leak.
  std::int64_t PruneCovers();

  // ---- settings ----------------------------------------------------------
  //
  // A key/value table for the little that has to survive a restart and can be
  // rebuilt if it does not: which folder to scan, and nothing that a person
  // would miss. Anything they would miss does not belong in this file at all
  // (ADR 0007).

  void SetSetting(const std::string& key, const std::string& value);
  [[nodiscard]] std::optional<std::string> GetSetting(const std::string& key) const;

 private:
  // Called by the constructor, under the lock.
  void Migrate();
  void CreateSchemaV1();
  void MigrateToV2();
  [[nodiscard]] int UserVersion() const;

  mutable std::mutex mutex_;
  sqlite3* database_ = nullptr;
};

// Identifies a picture by its content.
//
// FNV-1a, 64 bits, not a cryptographic hash and not trying to be: this is a
// cache key for pictures that came out of the user's own files. Two different
// covers colliding would need roughly a hundred million distinct images before
// it became likely, a music library has thousands, and the cost of being wrong
// is one album showing another album's artwork until the next scan. A SHA would
// buy certainty at the price of a dependency and ten times the work per file.
[[nodiscard]] std::string CoverHash(const std::vector<std::uint8_t>& bytes);

namespace detail {

// Turns what somebody typed into an FTS5 MATCH expression.
//
// This is the function that stops a quote in the search box from being a syntax
// error -- or worse, a query. FTS5 has its own grammar: NEAR, AND, a colon to
// pick a column, a quote to start a phrase. A search box that passes its
// contents through unchanged breaks on the apostrophe in "Livin' on a Prayer",
// which is not a rare name shape in a music library.
//
// So every word is wrapped in double quotes, with any quote inside it doubled,
// which makes it a literal phrase and takes every operator out of play. The last
// word gets a * because the person is probably still typing it.
[[nodiscard]] std::string MakeMatchQuery(const std::string& text);

}  // namespace detail

}  // namespace sonora::library
