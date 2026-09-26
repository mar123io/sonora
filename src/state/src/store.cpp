#include <sonora/state/store.h>

#include <sqlite3.h>

#include <string_view>
#include <utility>

namespace sonora::state {
namespace {

// These four helpers are close cousins of the ones in src/library/src/library.cpp
// and they are deliberately not shared.
//
// What would have to be shared with them is the exception type, and that is the
// one thing these two stores must not have in common: the shell catches
// LibraryError to turn a failed query into a bridge error the page can show,
// and a failure in here is not that -- it is a failure of something the user
// cannot see and did not ask for. A third target existing only to hold a common
// exception would make the two look alike in exactly the place they differ.
// Ninety lines of visible duplication beats a shared base class that says
// something untrue.

void Execute(sqlite3* database, const char* sql) {
  char* message = nullptr;
  if (sqlite3_exec(database, sql, nullptr, nullptr, &message) != SQLITE_OK) {
    const std::string text = message != nullptr ? message : "unknown error";
    sqlite3_free(message);
    throw StateError("cannot execute: " + text);
  }
}

class Statement {
 public:
  Statement(sqlite3* database, std::string_view sql) : database_(database) {
    const int result = sqlite3_prepare_v2(database, sql.data(), static_cast<int>(sql.size()),
                                          &statement_, nullptr);
    if (result != SQLITE_OK) {
      throw StateError("cannot prepare: " + std::string(sqlite3_errmsg(database)) + " in [" +
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

  Statement& Bind(int index, const std::string& value) {
    Check(sqlite3_bind_text(statement_, index, value.c_str(), static_cast<int>(value.size()),
                            SQLITE_TRANSIENT),
          "bind");
    return *this;
  }

  bool Step() {
    const int result = sqlite3_step(statement_);
    if (result == SQLITE_ROW) {
      return true;
    }
    if (result == SQLITE_DONE) {
      return false;
    }
    throw StateError("cannot step: " + std::string(sqlite3_errmsg(database_)));
  }

  void Run() {
    while (Step()) {
    }
  }

  [[nodiscard]] sqlite3_stmt* get() const noexcept { return statement_; }

 private:
  void Check(int result, const char* what) {
    if (result != SQLITE_OK) {
      throw StateError(std::string("cannot ") + what + ": " + sqlite3_errmsg(database_));
    }
  }

  sqlite3* database_;
  sqlite3_stmt* statement_ = nullptr;
};

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

[[nodiscard]] std::string Text(sqlite3_stmt* statement, int column) {
  const unsigned char* value = sqlite3_column_text(statement, column);
  if (value == nullptr) {
    return {};
  }
  return std::string(reinterpret_cast<const char*>(value),
                     static_cast<std::size_t>(sqlite3_column_bytes(statement, column)));
}

[[nodiscard]] std::string ToUtf8(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return std::string(text.begin(), text.end());
}

[[nodiscard]] PlayRecord ReadRecord(sqlite3_stmt* statement) {
  PlayRecord record;
  record.id = sqlite3_column_int64(statement, 0);
  record.path = Text(statement, 1);
  record.last_played_at = sqlite3_column_int64(statement, 2);
  record.play_count = sqlite3_column_int64(statement, 3);
  return record;
}

constexpr const char* kColumns = "id, path, last_played_at, play_count";

}  // namespace

Store::Store(const std::filesystem::path& database_path) {
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
    throw StateError("cannot open '" + utf8 + "': " + message);
  }

  sqlite3_busy_timeout(database_, 5000);

  // The try is not decoration. A constructor that throws has no destructor
  // called, so an exception from Migrate() -- which is exactly what a store
  // written by a newer Sonora produces -- would leave this connection open for
  // the life of the process, holding a lock on a file the user was just told to
  // go and look at. LeakSanitizer found this shape in Library first.
  try {
    const std::lock_guard<std::mutex> lock(mutex_);
    Execute(database_, "PRAGMA journal_mode = WAL");
    // FULL, and this is the line that makes this store a different kind of
    // thing from the index next to it. The index uses NORMAL because losing its
    // last write costs a rescan. Losing the last write here costs something
    // that was never anywhere else.
    Execute(database_, "PRAGMA synchronous = FULL");
    Migrate();
  } catch (...) {
    sqlite3_close(database_);
    database_ = nullptr;
    throw;
  }
}

Store::~Store() {
  sqlite3_close(database_);
}

int Store::UserVersion() const {
  Statement statement(database_, "PRAGMA user_version");
  if (!statement.Step()) {
    throw StateError("PRAGMA user_version returned nothing");
  }
  return sqlite3_column_int(statement.get(), 0);
}

void Store::Migrate() {
  const int version = UserVersion();
  if (version == kSchemaVersion) {
    return;
  }
  if (version > kSchemaVersion) {
    // The index answers this case by offering to be deleted. This one cannot:
    // there is no rescan that rebuilds it. So it refuses, loudly, and leaves
    // the file exactly as it found it.
    throw StateError("this store was written by a newer version of Sonora (schema " +
                     std::to_string(version) + ", this build understands " +
                     std::to_string(kSchemaVersion) + ")");
  }

  Transaction transaction(database_);
  if (version < 1) {
    CreateSchemaV1();
  }
  Execute(database_, ("PRAGMA user_version = " + std::to_string(kSchemaVersion)).c_str());
  transaction.Commit();
}

void Store::CreateSchemaV1() {
  // INTEGER PRIMARY KEY without AUTOINCREMENT would let sqlite reuse the id of a
  // deleted row, and these ids are handed to the operating system: a jump-list
  // entry outlives this process, this index and several releases. AUTOINCREMENT
  // costs one extra table and buys the promise that a number means one file
  // forever.
  Execute(database_,
          "CREATE TABLE files ("
          "  id             INTEGER PRIMARY KEY AUTOINCREMENT,"
          "  path           TEXT NOT NULL UNIQUE,"
          "  last_played_at INTEGER NOT NULL DEFAULT 0,"
          "  play_count     INTEGER NOT NULL DEFAULT 0)");

  // The one query that has to be fast is "the most recent N", and it runs while
  // a window is opening.
  Execute(database_, "CREATE INDEX files_by_played ON files (last_played_at DESC)");
}

std::int64_t Store::IdForPath(const std::string& path) {
  if (path.empty()) {
    throw StateError("a file with no path cannot be remembered");
  }

  const std::lock_guard<std::mutex> lock(mutex_);
  // ON CONFLICT DO NOTHING followed by a select, rather than INSERT OR IGNORE
  // plus last_insert_rowid: the rowid of an insert that did nothing is the
  // rowid of whatever was inserted before it, which is a wrong answer that
  // looks like a right one.
  Statement insert(database_,
                   "INSERT INTO files (path) VALUES (?1) "
                   "ON CONFLICT (path) DO NOTHING");
  insert.Bind(1, path).Run();

  Statement select(database_, "SELECT id FROM files WHERE path = ?1");
  select.Bind(1, path);
  if (!select.Step()) {
    throw StateError("the row just written is not there");
  }
  return sqlite3_column_int64(select.get(), 0);
}

std::optional<std::string> Store::PathForId(std::int64_t id) const {
  const std::lock_guard<std::mutex> lock(mutex_);
  Statement statement(database_, "SELECT path FROM files WHERE id = ?1");
  statement.Bind(1, id);
  if (!statement.Step()) {
    return std::nullopt;
  }
  return Text(statement.get(), 0);
}

std::int64_t Store::NotePlayed(const std::string& path, std::int64_t when_ms) {
  if (path.empty()) {
    throw StateError("a file with no path cannot be remembered");
  }

  const std::lock_guard<std::mutex> lock(mutex_);
  // One statement, so the insert and the update cannot disagree. MAX() on the
  // timestamp because a clock can go backwards -- an NTP correction, a machine
  // waking up -- and "most recently played" going backwards in a list is the
  // kind of thing that gets explained as a ghost.
  Statement upsert(database_,
                   "INSERT INTO files (path, last_played_at, play_count) VALUES (?1, ?2, 1) "
                   "ON CONFLICT (path) DO UPDATE SET "
                   "  last_played_at = MAX(files.last_played_at, excluded.last_played_at),"
                   "  play_count = files.play_count + 1");
  upsert.Bind(1, path).Bind(2, when_ms).Run();

  Statement select(database_, "SELECT id FROM files WHERE path = ?1");
  select.Bind(1, path);
  if (!select.Step()) {
    throw StateError("the row just written is not there");
  }
  return sqlite3_column_int64(select.get(), 0);
}

std::vector<PlayRecord> Store::RecentlyPlayed(int limit) const {
  if (limit <= 0) {
    return {};
  }

  const std::lock_guard<std::mutex> lock(mutex_);
  Statement statement(database_, std::string("SELECT ") + kColumns +
                                     " FROM files WHERE last_played_at > 0 "
                                     "ORDER BY last_played_at DESC, id DESC LIMIT ?1");
  statement.Bind(1, static_cast<std::int64_t>(limit));

  std::vector<PlayRecord> records;
  while (statement.Step()) {
    records.push_back(ReadRecord(statement.get()));
  }
  return records;
}

std::optional<PlayRecord> Store::Find(const std::string& path) const {
  const std::lock_guard<std::mutex> lock(mutex_);
  Statement statement(database_,
                      std::string("SELECT ") + kColumns + " FROM files WHERE path = ?1");
  statement.Bind(1, path);
  if (!statement.Step()) {
    return std::nullopt;
  }
  return ReadRecord(statement.get());
}

std::int64_t Store::Count() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  Statement statement(database_, "SELECT COUNT(*) FROM files");
  if (!statement.Step()) {
    return 0;
  }
  return sqlite3_column_int64(statement.get(), 0);
}

std::int64_t Store::Forget(const std::vector<std::string>& paths) {
  if (paths.empty()) {
    return 0;
  }

  const std::lock_guard<std::mutex> lock(mutex_);
  Transaction transaction(database_);
  Statement remove(database_, "DELETE FROM files WHERE path = ?1");

  std::int64_t removed = 0;
  for (const std::string& path : paths) {
    sqlite3_reset(remove.get());
    sqlite3_clear_bindings(remove.get());
    remove.Bind(1, path).Run();
    removed += sqlite3_changes(database_);
  }
  transaction.Commit();
  return removed;
}

}  // namespace sonora::state
