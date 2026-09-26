#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <sonora/state/store.h>

using namespace sonora::state;

namespace {

// A temporary file, because half of what this store promises is only observable
// across a close and a reopen.
class TemporaryDatabase {
 public:
  TemporaryDatabase() {
    path_ = std::filesystem::temp_directory_path() /
            ("sonora-state-test-" + std::to_string(::rand()) + ".sqlite");
  }
  ~TemporaryDatabase() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
    std::filesystem::remove(path_.string() + "-wal", ignored);
    std::filesystem::remove(path_.string() + "-shm", ignored);
  }

  TemporaryDatabase(const TemporaryDatabase&) = delete;
  TemporaryDatabase& operator=(const TemporaryDatabase&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

}  // namespace

TEST_CASE("an id is issued once per path", "[state]") {
  Store store(":memory:");
  const std::int64_t first = store.IdForPath("/music/a.flac");
  REQUIRE(first > 0);
  REQUIRE(store.IdForPath("/music/a.flac") == first);
  REQUIRE(store.IdForPath("/music/b.flac") != first);
  REQUIRE(store.Count() == 2);
}

TEST_CASE("an id survives the process that issued it", "[state]") {
  // The reason this store exists at all: a jump-list entry registered with
  // Windows is still there next week, and the URL in it has to still mean the
  // same song.
  TemporaryDatabase database;
  std::int64_t id = 0;
  {
    Store store(database.path());
    id = store.NotePlayed("/music/a.flac", 1000);
  }
  {
    Store store(database.path());
    const auto path = store.PathForId(id);
    REQUIRE(path.has_value());
    REQUIRE(*path == "/music/a.flac");
  }
}

TEST_CASE("an id is never reused", "[state]") {
  // AUTOINCREMENT, and the test that says why it is worth an extra table:
  // without it sqlite hands the next insert the id of the row that was deleted,
  // and an old link starts playing a different song.
  TemporaryDatabase database;
  std::int64_t first = 0;
  {
    Store store(database.path());
    first = store.IdForPath("/music/a.flac");
    REQUIRE(store.Forget({"/music/a.flac"}) == 1);
  }
  {
    Store store(database.path());
    REQUIRE(store.IdForPath("/music/b.flac") != first);
    REQUIRE_FALSE(store.PathForId(first).has_value());
  }
}

TEST_CASE("an unknown id is empty, not an error", "[state]") {
  // What a link from an old jump list looks like after the music moved. The
  // shell shows nothing; it does not fail to start.
  Store store(":memory:");
  REQUIRE_FALSE(store.PathForId(12345).has_value());
  REQUIRE_FALSE(store.PathForId(0).has_value());
  REQUIRE_FALSE(store.PathForId(-1).has_value());
}

TEST_CASE("playing something records when and how often", "[state]") {
  Store store(":memory:");
  store.NotePlayed("/music/a.flac", 1000);
  store.NotePlayed("/music/a.flac", 2000);

  const auto record = store.Find("/music/a.flac");
  REQUIRE(record.has_value());
  REQUIRE(record->play_count == 2);
  REQUIRE(record->last_played_at == 2000);
}

TEST_CASE("a clock that goes backwards does not rewrite history", "[state]") {
  // An NTP correction, or a machine that woke up believing it was yesterday.
  // The count still goes up -- it was played -- but the list does not reorder
  // itself around a timestamp from the past.
  Store store(":memory:");
  store.NotePlayed("/music/a.flac", 5000);
  store.NotePlayed("/music/a.flac", 1000);

  const auto record = store.Find("/music/a.flac");
  REQUIRE(record->last_played_at == 5000);
  REQUIRE(record->play_count == 2);
}

TEST_CASE("the recent list is most recent first and excludes what was never played",
          "[state]") {
  Store store(":memory:");
  store.IdForPath("/music/never.flac");  // known, never played
  store.NotePlayed("/music/old.flac", 1000);
  store.NotePlayed("/music/new.flac", 3000);
  store.NotePlayed("/music/middle.flac", 2000);

  const std::vector<PlayRecord> recent = store.RecentlyPlayed(10);
  REQUIRE(recent.size() == 3);
  REQUIRE(recent[0].path == "/music/new.flac");
  REQUIRE(recent[1].path == "/music/middle.flac");
  REQUIRE(recent[2].path == "/music/old.flac");
}

TEST_CASE("the recent list honours its limit", "[state]") {
  Store store(":memory:");
  for (int i = 0; i < 20; ++i) {
    store.NotePlayed("/music/" + std::to_string(i) + ".flac", 1000 + i);
  }
  REQUIRE(store.RecentlyPlayed(5).size() == 5);
  REQUIRE(store.RecentlyPlayed(5).front().path == "/music/19.flac");
  REQUIRE(store.RecentlyPlayed(0).empty());
  REQUIRE(store.RecentlyPlayed(-3).empty());
}

TEST_CASE("two plays at the same millisecond still have an order", "[state]") {
  // Queueing an album plays several tracks within the same timer tick in the
  // shell's own tests, and a list whose order depends on what sqlite felt like
  // is a list that flickers.
  Store store(":memory:");
  store.NotePlayed("/music/a.flac", 1000);
  store.NotePlayed("/music/b.flac", 1000);

  const auto recent = store.RecentlyPlayed(10);
  REQUIRE(recent.size() == 2);
  REQUIRE(recent[0].path == "/music/b.flac");  // the later insert wins the tie
}

TEST_CASE("forgetting removes only what was named", "[state]") {
  Store store(":memory:");
  store.NotePlayed("/music/a.flac", 1000);
  store.NotePlayed("/music/b.flac", 2000);

  REQUIRE(store.Forget({"/music/a.flac", "/music/nothing-here.flac"}) == 1);
  REQUIRE(store.Count() == 1);
  REQUIRE(store.Find("/music/b.flac").has_value());
  REQUIRE(store.Forget({}) == 0);
}

TEST_CASE("a path is required", "[state]") {
  Store store(":memory:");
  REQUIRE_THROWS_AS(store.IdForPath(""), StateError);
  REQUIRE_THROWS_AS(store.NotePlayed("", 1000), StateError);
}

TEST_CASE("a store from the future is refused rather than guessed at", "[state]") {
  // The index can answer this case by offering to be deleted; this one cannot,
  // because nothing rebuilds it. So it has to refuse, and leave the file alone.
  //
  // The version is written into the file by hand because nothing in the public
  // interface can produce a newer schema -- that is the point of it. Offset 60
  // of the header is `user_version`, four bytes, big-endian: it is part of the
  // documented on-disk format, and after a clean close the WAL is checkpointed
  // away, so the main file's header is the one that counts.
  TemporaryDatabase database;
  {
    Store store(database.path());
    store.NotePlayed("/music/a.flac", 1000);
  }

  {
    std::fstream file(database.path(), std::ios::in | std::ios::out | std::ios::binary);
    REQUIRE(file.is_open());
    const char future[4] = {0, 0, 0, 99};
    file.seekp(60);
    file.write(future, sizeof(future));
    REQUIRE(file.good());
  }

  REQUIRE_THROWS_AS(Store(database.path()), StateError);
}

TEST_CASE("unicode paths survive the round trip", "[state]") {
  // Paths are UTF-8 above the platform layer, and a music folder is where the
  // characters that break that assumption live.
  Store store(":memory:");
  const std::string path = "/music/Björk/Homogénic/01 Hunter.flac";
  const std::int64_t id = store.NotePlayed(path, 1000);
  REQUIRE(store.PathForId(id) == path);
}
