#include <sonora/library/library.h>

#include <sqlite3.h>

#include <cctype>
#include <cstdio>
#include <string_view>
#include <utility>

namespace sonora::library {
namespace {

// The columns of `tracks`, in one place, in the order the reader below expects.
// Two lists that have to agree are one list too many.
//
// Qualified with the table name because the search query joins `tracks` to
// `tracks_fts`, and both have a column called title.
constexpr const char* kTrackColumns =
    "tracks.id, tracks.path, tracks.title, tracks.artist, tracks.album, "
    "tracks.album_artist, tracks.track_number, tracks.disc_number, tracks.year, "
    "tracks.duration_ms, tracks.cover_hash, tracks.mtime_ns, tracks.size_bytes";

constexpr const char* kTrackOrder = "album_artist, album, disc_number, track_number, title";

[[nodiscard]] std::string ToUtf8(const std::filesystem::path& path) {
  // std::filesystem::path is UTF-16 on Windows and sqlite3_open takes UTF-8, so
  // the conversion happens here, at the edge of this target, and nothing above
  // this line has to think about it.
  const std::u8string text = path.u8string();
  return std::string(text.begin(), text.end());
}

[[nodiscard]] std::string Text(sqlite3_stmt* statement, int column) {
  // A TEXT column that is NULL gives a null pointer, and std::string does not
  // survive being constructed from one. The schema says NOT NULL everywhere, so
  // this cannot happen -- and it costs one branch to make sure that a schema
  // change never turns into a crash in a list view.
  const unsigned char* value = sqlite3_column_text(statement, column);
  if (value == nullptr) {
    return {};
  }
  return std::string(reinterpret_cast<const char*>(value),
                     static_cast<std::size_t>(sqlite3_column_bytes(statement, column)));
}

[[nodiscard]] Track ReadTrack(sqlite3_stmt* statement) {
  Track track;
  track.id = sqlite3_column_int64(statement, 0);
  track.path = Text(statement, 1);
  track.title = Text(statement, 2);
  track.artist = Text(statement, 3);
  track.album = Text(statement, 4);
  track.album_artist = Text(statement, 5);
  track.track_number = sqlite3_column_int(statement, 6);
  track.disc_number = sqlite3_column_int(statement, 7);
  track.year = sqlite3_column_int(statement, 8);
  track.duration_ms = sqlite3_column_int64(statement, 9);
  track.cover_hash = Text(statement, 10);
  track.mtime_ns = sqlite3_column_int64(statement, 11);
  track.size_bytes = sqlite3_column_int64(statement, 12);
  return track;
}

// A prepared statement that finalizes itself, and an error that says which
// statement failed.
//
// Prepared once per call rather than cached: sqlite3_prepare_v2 on these
// statements is microseconds, and the thing it is being compared against is
// reading the tags out of a FLAC, which is milliseconds. Caching would buy a
// tenth of a percent of a scan and cost a map keyed by SQL text.
class Statement {
 public:
  Statement(sqlite3* database, std::string_view sql) : database_(database) {
    const int result = sqlite3_prepare_v2(database, sql.data(), static_cast<int>(sql.size()),
                                          &statement_, nullptr);
    if (result != SQLITE_OK) {
      throw LibraryError("cannot prepare: " + std::string(sqlite3_errmsg(database)) + " in [" +
                         std::string(sql) + "]");
    }
  }

  ~Statement() { sqlite3_finalize(statement_); }

  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;

  Statement& Bind(int index, std::int64_t value) {
    Check(sqlite3_bind_int64(statement_, index, value), "bind");
    return *this;
  }

  Statement& Bind(int index, const std::vector<std::uint8_t>& value) {
    // A cover is a few hundred kilobytes and sqlite copies it. The alternative,
    // SQLITE_STATIC, would mean the vector has to outlive the step -- true here
    // today and a use-after-free the first time a caller inlines a temporary.
    Check(sqlite3_bind_blob(statement_, index, value.data(), static_cast<int>(value.size()),
                            SQLITE_TRANSIENT),
          "bind blob");
    return *this;
  }

  Statement& Bind(int index, const std::string& value) {
    // SQLITE_TRANSIENT: sqlite copies. The alternative is promising that every
    // string outlives every Step(), which is a promise a caller forgets once.
    Check(sqlite3_bind_text(statement_, index, value.c_str(), static_cast<int>(value.size()),
                            SQLITE_TRANSIENT),
          "bind");
    return *this;
  }

  // True when a row is available, false at the end. Anything else throws.
  bool Step() {
    const int result = sqlite3_step(statement_);
    if (result == SQLITE_ROW) {
      return true;
    }
    if (result == SQLITE_DONE) {
      return false;
    }
    throw LibraryError("cannot step: " + std::string(sqlite3_errmsg(database_)));
  }

  void Run() {
    while (Step()) {
    }
  }

  [[nodiscard]] sqlite3_stmt* get() const noexcept { return statement_; }

 private:
  void Check(int result, const char* what) const {
    if (result != SQLITE_OK) {
      throw LibraryError(std::string("cannot ") + what + ": " + sqlite3_errmsg(database_));
    }
  }

  sqlite3* database_;
  sqlite3_stmt* statement_ = nullptr;
};

void Execute(sqlite3* database, const char* sql) {
  char* message = nullptr;
  if (sqlite3_exec(database, sql, nullptr, nullptr, &message) != SQLITE_OK) {
    const std::string text = message != nullptr ? message : "unknown error";
    sqlite3_free(message);
    throw LibraryError("cannot execute: " + text);
  }
}

// BEGIN ... COMMIT, with a ROLLBACK if the scope is left by an exception.
//
// Without it a failed batch leaves half of itself written, and the next scan
// would see those files as already indexed. The scanner would then never look at
// them again, because their mtime has not changed -- a silent, permanent hole in
// the index, from one bad file.
class Transaction {
 public:
  explicit Transaction(sqlite3* database) : database_(database) {
    Execute(database_, "BEGIN IMMEDIATE");
  }

  ~Transaction() {
    if (!committed_) {
      char* message = nullptr;
      sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, &message);
      sqlite3_free(message);
    }
  }

  Transaction(const Transaction&) = delete;
  Transaction& operator=(const Transaction&) = delete;

  void Commit() {
    Execute(database_, "COMMIT");
    committed_ = true;
  }

 private:
  sqlite3* database_;
  bool committed_ = false;
};

void BindTrack(Statement& statement, const Track& track) {
  statement.Bind(1, track.path)
      .Bind(2, track.title)
      .Bind(3, track.artist)
      .Bind(4, track.album)
      .Bind(5, track.album_artist)
      .Bind(6, static_cast<std::int64_t>(track.track_number))
      .Bind(7, static_cast<std::int64_t>(track.disc_number))
      .Bind(8, static_cast<std::int64_t>(track.year))
      .Bind(9, track.duration_ms)
      .Bind(10, track.cover_hash)
      .Bind(11, track.mtime_ns)
      .Bind(12, track.size_bytes);
}

constexpr const char* kUpsertSql =
    "INSERT INTO tracks (path, title, artist, album, album_artist, track_number, "
    "disc_number, year, duration_ms, cover_hash, mtime_ns, size_bytes) "
    "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12) "
    "ON CONFLICT(path) DO UPDATE SET "
    "title=excluded.title, artist=excluded.artist, album=excluded.album, "
    "album_artist=excluded.album_artist, track_number=excluded.track_number, "
    "disc_number=excluded.disc_number, year=excluded.year, "
    "duration_ms=excluded.duration_ms, cover_hash=excluded.cover_hash, "
    "mtime_ns=excluded.mtime_ns, size_bytes=excluded.size_bytes "
    "RETURNING id";

}  // namespace

Library::Library(const std::filesystem::path& database_path) {
  std::string utf8 = ToUtf8(database_path);
  if (utf8.empty()) {
    utf8 = ":memory:";
  }

  const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
  if (sqlite3_open_v2(utf8.c_str(), &database_, flags, nullptr) != SQLITE_OK) {
    const std::string message =
        database_ != nullptr ? sqlite3_errmsg(database_) : "out of memory";
    sqlite3_close(database_);
    database_ = nullptr;
    throw LibraryError("cannot open '" + utf8 + "': " + message);
  }

  // A query that has to wait for a scan's write batch waits for it rather than
  // failing. Five seconds is far longer than any batch in here takes; it is a
  // bound, not an expectation.
  sqlite3_busy_timeout(database_, 5000);

  const std::lock_guard<std::mutex> lock(mutex_);
  Execute(database_, "PRAGMA foreign_keys = ON");
  // WAL so a read never blocks the write and the other way round. It is a no-op
  // for ":memory:", which is why the tests still exercise the rest of this.
  Execute(database_, "PRAGMA journal_mode = WAL");
  // NORMAL rather than FULL: losing the last batch of a scan to a power cut
  // costs one rescan of a few files. See the class comment -- this file is a
  // cache.
  Execute(database_, "PRAGMA synchronous = NORMAL");
  Migrate();
}

Library::~Library() {
  sqlite3_close(database_);
}

int Library::UserVersion() const {
  Statement statement(database_, "PRAGMA user_version");
  if (!statement.Step()) {
    throw LibraryError("PRAGMA user_version returned nothing");
  }
  return sqlite3_column_int(statement.get(), 0);
}

void Library::Migrate() {
  const int version = UserVersion();
  if (version == kSchemaVersion) {
    return;
  }
  if (version > kSchemaVersion) {
    // A newer Sonora has written this file. Guessing what its columns mean is
    // how an index gets silently corrupted; saying so is how a person gets to
    // choose.
    throw LibraryError("this library was written by a newer version of Sonora (schema " +
                       std::to_string(version) + ", this build understands " +
                       std::to_string(kSchemaVersion) + ")");
  }

  Transaction transaction(database_);
  // A ladder, walked from wherever this file is to where this build expects it.
  // A brand new database walks every rung -- it does not get a shortcut to the
  // final shape -- which is what keeps the migration path exercised by every
  // run instead of only by an upgrade nobody has to hand to test.
  if (version < 1) {
    CreateSchemaV1();
  }
  if (version < 2) {
    MigrateToV2();
  }
  Execute(database_, ("PRAGMA user_version = " + std::to_string(kSchemaVersion)).c_str());
  transaction.Commit();
}

void Library::CreateSchemaV1() {
  Execute(database_,
          "CREATE TABLE tracks ("
          "  id           INTEGER PRIMARY KEY,"
          "  path         TEXT    NOT NULL UNIQUE,"
          "  title        TEXT    NOT NULL DEFAULT '',"
          "  artist       TEXT    NOT NULL DEFAULT '',"
          "  album        TEXT    NOT NULL DEFAULT '',"
          "  album_artist TEXT    NOT NULL DEFAULT '',"
          "  track_number INTEGER NOT NULL DEFAULT 0,"
          "  disc_number  INTEGER NOT NULL DEFAULT 0,"
          "  year         INTEGER NOT NULL DEFAULT 0,"
          "  duration_ms  INTEGER NOT NULL DEFAULT 0,"
          "  mtime_ns     INTEGER NOT NULL DEFAULT 0,"
          "  size_bytes   INTEGER NOT NULL DEFAULT 0)");

  // The two orderings the UI actually asks for. Without them, every album list
  // is a sort of the whole table.
  Execute(database_,
          "CREATE INDEX tracks_by_album ON tracks (album_artist, album, "
          "disc_number, track_number)");
  Execute(database_, "CREATE INDEX tracks_by_artist ON tracks (artist, album, track_number)");

  // FTS5, external content: the text lives in `tracks` and this table holds only
  // the inverted index. The alternative -- a contentless or a standalone FTS
  // table -- would mean storing every title twice and keeping the copies in
  // step.
  //
  // remove_diacritics 2 is not a detail in an Italian music library: it is what
  // makes "cosi" find "Così" and "bjork" find "Björk". Somebody searching has a
  // keyboard, not the tag.
  const char* kCreateFts =
      "CREATE VIRTUAL TABLE tracks_fts USING fts5("
      "  title, artist, album, album_artist,"
      "  content='tracks', content_rowid='id',"
      "  tokenize=\"unicode61 remove_diacritics 2\")";
  char* message = nullptr;
  if (sqlite3_exec(database_, kCreateFts, nullptr, nullptr, &message) != SQLITE_OK) {
    const std::string text = message != nullptr ? message : "unknown error";
    sqlite3_free(message);
    // The likeliest cause by a wide margin is a SQLite built without FTS5,
    // which on this project means the vcpkg dependency was written as "sqlite3"
    // instead of asking for the feature. Saying that here saves the next person
    // an hour.
    throw LibraryError("cannot create the search index: " + text +
                       " -- is this SQLite built with FTS5? "
                       "vcpkg.json must request the sqlite3 \"fts5\" feature");
  }

  // With external content, sqlite does not keep the index in step by itself:
  // these three triggers are the contract. The delete half has to be given the
  // *old* values, because that is what the index has stored.
  Execute(database_,
          "CREATE TRIGGER tracks_fts_insert AFTER INSERT ON tracks BEGIN"
          "  INSERT INTO tracks_fts (rowid, title, artist, album, album_artist)"
          "  VALUES (new.id, new.title, new.artist, new.album, new.album_artist);"
          "END");
  Execute(database_,
          "CREATE TRIGGER tracks_fts_delete AFTER DELETE ON tracks BEGIN"
          "  INSERT INTO tracks_fts (tracks_fts, rowid, title, artist, album, album_artist)"
          "  VALUES ('delete', old.id, old.title, old.artist, old.album, old.album_artist);"
          "END");
  Execute(database_,
          "CREATE TRIGGER tracks_fts_update AFTER UPDATE ON tracks BEGIN"
          "  INSERT INTO tracks_fts (tracks_fts, rowid, title, artist, album, album_artist)"
          "  VALUES ('delete', old.id, old.title, old.artist, old.album, old.album_artist);"
          "  INSERT INTO tracks_fts (rowid, title, artist, album, album_artist)"
          "  VALUES (new.id, new.title, new.artist, new.album, new.album_artist);"
          "END");
}

void Library::MigrateToV2() {
  // Covers, and the column that points at them.
  //
  // ALTER TABLE ADD COLUMN with a default is cheap in SQLite -- it rewrites the
  // header, not the rows -- so an existing index of fifty thousand tracks gains
  // the column in milliseconds and then fills it in on the next scan, because
  // the artwork is a fact about the file and the scan is what reads those.
  Execute(database_, "ALTER TABLE tracks ADD COLUMN cover_hash TEXT NOT NULL DEFAULT ''");

  // The bytes live once per distinct image. Twelve tracks of an album carry the
  // same JPEG, and before this table that was twelve copies.
  Execute(database_,
          "CREATE TABLE covers ("
          "  hash  TEXT PRIMARY KEY,"
          "  mime  TEXT NOT NULL,"
          "  bytes BLOB NOT NULL)");

  // For the prune, and for the album list's cover lookup.
  Execute(database_, "CREATE INDEX tracks_by_cover ON tracks (cover_hash)");

  // Small, boring, and not user data: the folder to scan, so the next start does
  // not have to be told again. See ADR 0007 on what must never be kept here.
  Execute(database_,
          "CREATE TABLE settings ("
          "  key   TEXT PRIMARY KEY,"
          "  value TEXT NOT NULL)");
}

std::int64_t Library::Upsert(const Track& track) {
  const std::lock_guard<std::mutex> lock(mutex_);
  Statement statement(database_, kUpsertSql);
  BindTrack(statement, track);
  if (!statement.Step()) {
    throw LibraryError("upsert returned no id for '" + track.path + "'");
  }
  return sqlite3_column_int64(statement.get(), 0);
}

void Library::UpsertBatch(const std::vector<Track>& tracks) {
  if (tracks.empty()) {
    return;
  }
  const std::lock_guard<std::mutex> lock(mutex_);
  Transaction transaction(database_);
  for (const Track& track : tracks) {
    Statement statement(database_, kUpsertSql);
    BindTrack(statement, track);
    if (!statement.Step()) {
      throw LibraryError("upsert returned no id for '" + track.path + "'");
    }
  }
  transaction.Commit();
}

void Library::Remove(std::int64_t id) {
  const std::lock_guard<std::mutex> lock(mutex_);
  Statement statement(database_, "DELETE FROM tracks WHERE id = ?1");
  statement.Bind(1, id).Run();
}

void Library::RemoveBatch(const std::vector<std::int64_t>& ids) {
  if (ids.empty()) {
    return;
  }
  const std::lock_guard<std::mutex> lock(mutex_);
  Transaction transaction(database_);
  for (const std::int64_t id : ids) {
    Statement statement(database_, "DELETE FROM tracks WHERE id = ?1");
    statement.Bind(1, id).Run();
  }
  transaction.Commit();
}

std::optional<Track> Library::Get(std::int64_t id) const {
  const std::lock_guard<std::mutex> lock(mutex_);
  Statement statement(database_,
                      std::string("SELECT ") + kTrackColumns + " FROM tracks WHERE id = ?1");
  statement.Bind(1, id);
  if (!statement.Step()) {
    return std::nullopt;
  }
  return ReadTrack(statement.get());
}

std::vector<Track> Library::GetMany(const std::vector<std::int64_t>& ids) const {
  if (ids.empty()) {
    return {};
  }

  const std::lock_guard<std::mutex> lock(mutex_);
  // One statement, prepared once, stepped per id. The alternative -- an IN
  // clause built by pasting the ids into the SQL -- is how a query becomes an
  // injection, even when the values are integers this code produced itself.
  Statement statement(database_,
                      std::string("SELECT ") + kTrackColumns + " FROM tracks WHERE id = ?1");

  std::vector<Track> tracks;
  tracks.reserve(ids.size());
  for (const std::int64_t id : ids) {
    sqlite3_reset(statement.get());
    statement.Bind(1, id);
    if (statement.Step()) {
      // Order is the caller's, not the table's: enqueueing an album means
      // playing it in the order the page listed it.
      tracks.push_back(ReadTrack(statement.get()));
    }
  }
  return tracks;
}

std::optional<Track> Library::FindByPath(const std::string& path) const {
  const std::lock_guard<std::mutex> lock(mutex_);
  Statement statement(database_,
                      std::string("SELECT ") + kTrackColumns + " FROM tracks WHERE path = ?1");
  statement.Bind(1, path);
  if (!statement.Step()) {
    return std::nullopt;
  }
  return ReadTrack(statement.get());
}

std::int64_t Library::TrackCount() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  Statement statement(database_, "SELECT COUNT(*) FROM tracks");
  if (!statement.Step()) {
    return 0;
  }
  return sqlite3_column_int64(statement.get(), 0);
}

std::int64_t Library::AlbumCount() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  Statement statement(
      database_, "SELECT COUNT(*) FROM (SELECT 1 FROM tracks GROUP BY album, album_artist)");
  return statement.Step() ? sqlite3_column_int64(statement.get(), 0) : 0;
}

std::int64_t Library::ArtistCount() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  Statement statement(database_,
                      "SELECT COUNT(DISTINCT artist) FROM tracks WHERE artist <> ''");
  return statement.Step() ? sqlite3_column_int64(statement.get(), 0) : 0;
}

std::vector<Track> Library::ListTracks(std::int64_t limit, std::int64_t offset) const {
  const std::lock_guard<std::mutex> lock(mutex_);
  Statement statement(database_, std::string("SELECT ") + kTrackColumns +
                                     " FROM tracks ORDER BY " + kTrackOrder +
                                     " LIMIT ?1 OFFSET ?2");
  // sqlite reads a negative LIMIT as "no limit", which is exactly the meaning
  // this API documents, so there is no branch here.
  statement.Bind(1, limit).Bind(2, offset);

  std::vector<Track> tracks;
  while (statement.Step()) {
    tracks.push_back(ReadTrack(statement.get()));
  }
  return tracks;
}

std::vector<AlbumSummary> Library::ListAlbums() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  // MIN(year) rather than any year: a compilation retagged one track at a time
  // would otherwise change the album's year depending on which row sqlite
  // happened to read.
  //
  // MAX(cover_hash) is how the album gets a cover from whichever of its tracks
  // has one: an empty string sorts below every hash, so MAX picks a real one if
  // any track has artwork and '' only when none do. A correlated subquery would
  // say it more plainly and would run once per album instead of never.
  Statement statement(database_,
                      "SELECT album, album_artist, MIN(year), COUNT(*), SUM(duration_ms), "
                      "MAX(cover_hash) "
                      "FROM tracks GROUP BY album, album_artist "
                      "ORDER BY album_artist, album");

  std::vector<AlbumSummary> albums;
  while (statement.Step()) {
    AlbumSummary album;
    album.album = Text(statement.get(), 0);
    album.album_artist = Text(statement.get(), 1);
    album.year = sqlite3_column_int(statement.get(), 2);
    album.track_count = sqlite3_column_int(statement.get(), 3);
    album.duration_ms = sqlite3_column_int64(statement.get(), 4);
    album.cover_hash = Text(statement.get(), 5);
    albums.push_back(std::move(album));
  }
  return albums;
}

std::vector<std::string> Library::ListArtists() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  // The artist of the track, not of the album: on a compilation those differ,
  // and the sidebar is for finding a performer.
  Statement statement(database_,
                      "SELECT DISTINCT artist FROM tracks WHERE artist <> '' "
                      "ORDER BY artist");

  std::vector<std::string> artists;
  while (statement.Step()) {
    artists.push_back(Text(statement.get(), 0));
  }
  return artists;
}

std::vector<Track> Library::AlbumTracks(const std::string& album,
                                        const std::string& album_artist) const {
  const std::lock_guard<std::mutex> lock(mutex_);
  Statement statement(database_, std::string("SELECT ") + kTrackColumns +
                                     " FROM tracks WHERE album = ?1 AND album_artist = ?2 "
                                     "ORDER BY disc_number, track_number, title");
  statement.Bind(1, album).Bind(2, album_artist);

  std::vector<Track> tracks;
  while (statement.Step()) {
    tracks.push_back(ReadTrack(statement.get()));
  }
  return tracks;
}

std::vector<Track> Library::ArtistTracks(const std::string& artist) const {
  const std::lock_guard<std::mutex> lock(mutex_);
  Statement statement(database_, std::string("SELECT ") + kTrackColumns +
                                     " FROM tracks WHERE artist = ?1 "
                                     "ORDER BY album, disc_number, track_number, title");
  statement.Bind(1, artist);

  std::vector<Track> tracks;
  while (statement.Step()) {
    tracks.push_back(ReadTrack(statement.get()));
  }
  return tracks;
}

std::vector<Track> Library::Search(const std::string& text, std::int64_t limit) const {
  const std::string match = detail::MakeMatchQuery(text);
  if (match.empty()) {
    // An empty search box is not a search for everything; it is not a search.
    // Returning the whole library here would make the list flicker between two
    // completely different contents on every backspace.
    return {};
  }

  const std::lock_guard<std::mutex> lock(mutex_);
  Statement statement(database_,
                      std::string("SELECT ") + kTrackColumns +
                          " FROM tracks_fts JOIN tracks ON tracks.id = tracks_fts.rowid "
                          "WHERE tracks_fts MATCH ?1 ORDER BY rank LIMIT ?2");
  statement.Bind(1, match).Bind(2, limit);

  std::vector<Track> tracks;
  while (statement.Step()) {
    tracks.push_back(ReadTrack(statement.get()));
  }
  return tracks;
}

std::unordered_map<std::string, FileStamp> Library::Stamps() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  Statement statement(database_, "SELECT id, path, mtime_ns, size_bytes FROM tracks");

  std::unordered_map<std::string, FileStamp> stamps;
  while (statement.Step()) {
    FileStamp stamp;
    stamp.id = sqlite3_column_int64(statement.get(), 0);
    stamp.mtime_ns = sqlite3_column_int64(statement.get(), 2);
    stamp.size_bytes = sqlite3_column_int64(statement.get(), 3);
    stamps.emplace(Text(statement.get(), 1), stamp);
  }
  return stamps;
}

bool Library::HasCover(const std::string& hash) const {
  if (hash.empty()) {
    return false;
  }
  const std::lock_guard<std::mutex> lock(mutex_);
  Statement statement(database_, "SELECT 1 FROM covers WHERE hash = ?1");
  statement.Bind(1, hash);
  return statement.Step();
}

void Library::PutCover(const std::string& hash, const Cover& cover) {
  if (hash.empty() || cover.bytes.empty()) {
    return;
  }
  const std::lock_guard<std::mutex> lock(mutex_);
  // OR IGNORE rather than a check followed by an insert: the scan's workers all
  // read tags at once, and two of them finding the same album cover at the same
  // moment is the normal case, not a race to defend against.
  Statement statement(database_,
                      "INSERT OR IGNORE INTO covers (hash, mime, bytes) VALUES (?1,?2,?3)");
  statement.Bind(1, hash).Bind(2, cover.mime).Bind(3, cover.bytes).Run();
}

std::optional<Cover> Library::GetCover(const std::string& hash) const {
  if (hash.empty()) {
    return std::nullopt;
  }
  const std::lock_guard<std::mutex> lock(mutex_);
  Statement statement(database_, "SELECT mime, bytes FROM covers WHERE hash = ?1");
  statement.Bind(1, hash);
  if (!statement.Step()) {
    return std::nullopt;
  }

  Cover cover;
  cover.mime = Text(statement.get(), 0);
  const void* blob = sqlite3_column_blob(statement.get(), 1);
  const int size = sqlite3_column_bytes(statement.get(), 1);
  if (blob != nullptr && size > 0) {
    const auto* begin = static_cast<const std::uint8_t*>(blob);
    cover.bytes.assign(begin, begin + size);
  }
  return cover;
}

std::int64_t Library::CoverCount() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  Statement statement(database_, "SELECT COUNT(*) FROM covers");
  return statement.Step() ? sqlite3_column_int64(statement.get(), 0) : 0;
}

std::int64_t Library::PruneCovers() {
  const std::lock_guard<std::mutex> lock(mutex_);
  Statement statement(database_,
                      "DELETE FROM covers WHERE hash NOT IN "
                      "(SELECT cover_hash FROM tracks WHERE cover_hash <> '')");
  statement.Run();
  return sqlite3_changes(database_);
}

void Library::SetSetting(const std::string& key, const std::string& value) {
  const std::lock_guard<std::mutex> lock(mutex_);
  Statement statement(database_,
                      "INSERT INTO settings (key, value) VALUES (?1,?2) "
                      "ON CONFLICT(key) DO UPDATE SET value = excluded.value");
  statement.Bind(1, key).Bind(2, value).Run();
}

std::optional<std::string> Library::GetSetting(const std::string& key) const {
  const std::lock_guard<std::mutex> lock(mutex_);
  Statement statement(database_, "SELECT value FROM settings WHERE key = ?1");
  statement.Bind(1, key);
  if (!statement.Step()) {
    return std::nullopt;
  }
  return Text(statement.get(), 0);
}

std::string CoverHash(const std::vector<std::uint8_t>& bytes) {
  // FNV-1a, 64 bits. See the declaration for why this and not a real digest.
  std::uint64_t hash = 0xcbf29ce484222325ULL;
  for (const std::uint8_t byte : bytes) {
    hash ^= byte;
    hash *= 0x100000001b3ULL;
  }

  // The length goes into the key as well. It costs nothing and it means two
  // pictures have to agree on both the hash and the byte count to be mistaken
  // for each other.
  char buffer[33] = {};
  std::snprintf(buffer, sizeof(buffer), "%016llx%08llx", static_cast<unsigned long long>(hash),
                static_cast<unsigned long long>(bytes.size() & 0xffffffffULL));
  return std::string(buffer);
}

namespace detail {

std::string MakeMatchQuery(const std::string& text) {
  std::string query;
  std::size_t index = 0;

  while (index < text.size()) {
    // Bytes above 0x7f are the middle of a UTF-8 sequence and are part of the
    // word: splitting on ASCII whitespace only is what keeps "Così" one token.
    while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index])) != 0) {
      ++index;
    }
    const std::size_t begin = index;
    while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index])) == 0) {
      ++index;
    }
    if (begin == index) {
      break;
    }

    // A word the tokenizer would throw away entirely -- a lone quote, a dash,
    // an apostrophe on its own -- becomes an empty phrase, and an empty phrase
    // is an FTS5 syntax error. Dropping it here is why typing a quote into the
    // search box shows no results rather than an error.
    bool searchable = false;
    for (std::size_t i = begin; i < index && !searchable; ++i) {
      const unsigned char byte = static_cast<unsigned char>(text[i]);
      searchable = byte >= 0x80 || std::isalnum(byte) != 0;
    }
    if (!searchable) {
      continue;
    }

    if (!query.empty()) {
      query += ' ';  // FTS5 reads a space between phrases as AND.
    }
    query += '"';
    for (std::size_t i = begin; i < index; ++i) {
      if (text[i] == '"') {
        query += "\"\"";
      } else {
        query += text[i];
      }
    }
    query += '"';
  }

  if (!query.empty()) {
    // Prefix match on the last word only. Doing it to every word would make
    // "the best of" match half the library.
    query += '*';
  }
  return query;
}

}  // namespace detail

}  // namespace sonora::library
