#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

struct sqlite3;

namespace sonora::state {

class StateError : public std::runtime_error {
 public:
  explicit StateError(const std::string& message) : std::runtime_error(message) {}
};

// One file the user has a history with.
struct PlayRecord {
  // Stable for the life of the installation. Not a row of the library index:
  // that one is a cache and its ids are reassigned every time it is rebuilt.
  std::int64_t id = 0;
  std::string path;  // UTF-8
  std::int64_t last_played_at = 0;
  std::int64_t play_count = 0;
};

// The other store.
//
// ADR 0007 decided that the library index is a cache of the files and said, in
// as many words, that anything the user creates -- a playlist, a rating, a play
// count -- cannot live in something that may be deleted and rebuilt, and that
// week 9 would draw that boundary. This is the boundary.
//
// Three things make it a different kind of store rather than a second copy of
// the same one:
//
//   * **It keys by path, not by id.** The index's ids are cache-local: delete
//     the index, rescan, and every track has a new number. A durable store that
//     referred to those numbers would silently point at different music. The
//     path is the identity ADR 0007 already chose, and it is the only name the
//     two stores can share.
//
//   * **It issues stable ids of its own.** They are what a deep link carries.
//     A jump-list entry registered with Windows outlives the process, the
//     index, and the next three releases; a URL in it that meant track 412 last
//     month must not mean a different song today. These ids are never reused.
//
//   * **It is durable on purpose.** synchronous = FULL, where the index uses
//     NORMAL. Losing the last write of a scan costs a rescan of a few files;
//     losing what somebody played costs something nobody can reconstruct.
//
// What it holds today is the play history the jump list is built from. What it
// is *for* is everything of that shape that comes later.
//
// Threading: one mutex, one connection, like Library and for the same reasons.
class Store {
 public:
  static constexpr int kSchemaVersion = 1;

  // ":memory:" for tests.
  explicit Store(const std::filesystem::path& database_path);
  ~Store();

  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;

  // The durable id for a file, created on first use. Idempotent.
  std::int64_t IdForPath(const std::string& path);

  // The inverse. Empty when the id was never issued -- which is what a link
  // from an old jump list looks like after somebody moved their music.
  [[nodiscard]] std::optional<std::string> PathForId(std::int64_t id) const;

  // Records that this file was played, and returns its durable id.
  //
  // The timestamp is a parameter rather than a clock read inside: a store that
  // reads the clock cannot be tested for what it does at a particular moment,
  // and "most recently played" is entirely about particular moments.
  std::int64_t NotePlayed(const std::string& path, std::int64_t when_ms);

  // Most recent first, files never played excluded.
  [[nodiscard]] std::vector<PlayRecord> RecentlyPlayed(int limit) const;

  [[nodiscard]] std::optional<PlayRecord> Find(const std::string& path) const;
  [[nodiscard]] std::int64_t Count() const;

  // Removes the history of files that are no longer anywhere -- called with the
  // paths the index still knows about, after a scan.
  //
  // Not automatic, and not derived from the index's absence alone: a folder on
  // a drive that was not plugged in is missing from the index too, and forgetting
  // somebody's history because they booted without their external disk would be
  // exactly the kind of loss this store exists to prevent. The shell decides.
  std::int64_t Forget(const std::vector<std::string>& paths);

 private:
  void Migrate();
  void CreateSchemaV1();
  [[nodiscard]] int UserVersion() const;

  mutable std::mutex mutex_;
  sqlite3* database_ = nullptr;
};

}  // namespace sonora::state
