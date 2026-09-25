#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <sonora/library/library.h>
#include <sonora/library/scanner.h>
#include <sonora/library/tag_reader.h>

using namespace sonora::library;

namespace {

// The same conversion the scanner makes when it stores a path, so a test can ask
// the index about a file by name.
[[nodiscard]] std::string ToUtf8(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return std::string(text.begin(), text.end());
}

// A tag reader that never opens a file.
//
// The whole point of the TagReader interface: these tests are about what the
// scanner decides to read, and a real one would need real FLACs in the
// repository to decide anything about. Here the "tags" come from the file name,
// and the number that matters -- how many files were opened -- is counted.
class FakeTagReader final : public TagReader {
 public:
  std::optional<TagData> Read(const std::filesystem::path& path,
                              bool with_cover) const override {
    reads.fetch_add(1, std::memory_order_relaxed);
    if (with_cover) {
      cover_reads.fetch_add(1, std::memory_order_relaxed);
    }

    const std::string stem = path.stem().string();
    if (stem.rfind("broken", 0) == 0) {
      return std::nullopt;  // the half-copied file every music folder has
    }

    TagData tags;
    if (stem != "untitled") {
      tags.title = stem + " (title)";
    }
    tags.artist = "Artist";
    tags.album = "Album";
    tags.duration_ms = 1000;

    // Every file whose name starts with "art" carries the same picture, which is
    // what an album looks like: one cover, many tracks.
    if (with_cover && stem.rfind("art", 0) == 0) {
      Cover cover;
      cover.mime = "image/png";
      cover.bytes = {1, 2, 3, 4, 5, 6, 7, 8};
      tags.cover = cover;
    }
    // ...and this one carries a different picture, so dedup has something to
    // get wrong.
    if (with_cover && stem.rfind("other", 0) == 0) {
      Cover cover;
      cover.mime = "image/jpeg";
      cover.bytes = {9, 9, 9};
      tags.cover = cover;
    }
    return tags;
  }

  [[nodiscard]] std::string description() const override { return "fake"; }

  mutable std::atomic<int> reads{0};
  mutable std::atomic<int> cover_reads{0};
};

// A folder that deletes itself, because a test that leaves files behind in the
// temporary directory is a test that eventually fails on a full disk.
class TempTree {
 public:
  TempTree() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    root_ = std::filesystem::temp_directory_path() /
            ("sonora-scan-" + std::to_string(now) + "-" + std::to_string(counter_++));
    std::filesystem::create_directories(root_);
  }

  ~TempTree() {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  TempTree(const TempTree&) = delete;
  TempTree& operator=(const TempTree&) = delete;

  [[nodiscard]] const std::filesystem::path& root() const { return root_; }

  std::filesystem::path Write(const std::string& relative, const std::string& content) {
    const std::filesystem::path path = root_ / relative;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << content;
    file.close();
    return path;
  }

 private:
  static int counter_;
  std::filesystem::path root_;
};

int TempTree::counter_ = 0;

struct Fixture {
  TempTree tree;
  Library library{":memory:"};
  FakeTagReader* reader = nullptr;
  std::unique_ptr<Scanner> scanner;

  explicit Fixture(ScanOptions options = {}) {
    auto owned = std::make_unique<FakeTagReader>();
    reader = owned.get();
    scanner = std::make_unique<Scanner>(library, std::move(owned), std::move(options));
  }

  [[nodiscard]] int reads() const { return reader->reads.load(); }
  void ResetReads() const { reader->reads.store(0); }
};

}  // namespace

TEST_CASE("a first scan indexes every audio file it finds", "[scanner]") {
  Fixture fixture;
  fixture.tree.Write("a.flac", "aaaa");
  fixture.tree.Write("b.mp3", "bbbb");
  fixture.tree.Write("nested/deep/c.wav", "cccc");

  const ScanProgress progress = fixture.scanner->Scan(fixture.tree.root());

  REQUIRE(progress.done);
  REQUIRE_FALSE(progress.cancelled);
  REQUIRE(progress.files_seen == 3);
  REQUIRE(progress.files_read == 3);
  REQUIRE(progress.added == 3);
  REQUIRE(progress.updated == 0);
  REQUIRE(progress.removed == 0);
  REQUIRE(fixture.library.TrackCount() == 3);
  REQUIRE(fixture.library.Search("c").size() == 1);
}

TEST_CASE("a second scan of an unchanged folder reads nothing", "[scanner]") {
  // The property the whole design is for. If this test fails, scanning at
  // startup means re-reading every file in the music folder.
  Fixture fixture;
  fixture.tree.Write("a.flac", "aaaa");
  fixture.tree.Write("b.flac", "bbbb");

  fixture.scanner->Scan(fixture.tree.root());
  REQUIRE(fixture.reads() == 2);
  fixture.ResetReads();

  const ScanProgress again = fixture.scanner->Scan(fixture.tree.root());

  REQUIRE(fixture.reads() == 0);
  REQUIRE(again.files_seen == 2);
  REQUIRE(again.files_read == 0);
  REQUIRE(again.added == 0);
  REQUIRE(again.updated == 0);
  REQUIRE(again.removed == 0);
  REQUIRE(fixture.library.TrackCount() == 2);
}

TEST_CASE("a file whose size changed is read again", "[scanner]") {
  Fixture fixture;
  fixture.tree.Write("a.flac", "aaaa");
  fixture.scanner->Scan(fixture.tree.root());
  fixture.ResetReads();

  fixture.tree.Write("a.flac", "aaaaaaaaaaaa");
  const ScanProgress progress = fixture.scanner->Scan(fixture.tree.root());

  REQUIRE(fixture.reads() == 1);
  REQUIRE(progress.updated == 1);
  REQUIRE(progress.added == 0);
  REQUIRE(fixture.library.TrackCount() == 1);
}

TEST_CASE("a file whose modification time changed is read again", "[scanner]") {
  // Retagging a file usually leaves its size the same to the byte, so the
  // timestamp has to count on its own. Set explicitly rather than by rewriting,
  // because two writes in the same test can land inside one tick of the
  // filesystem's clock.
  Fixture fixture;
  const std::filesystem::path path = fixture.tree.Write("a.flac", "aaaa");
  fixture.scanner->Scan(fixture.tree.root());
  fixture.ResetReads();

  const std::filesystem::file_time_type when = std::filesystem::last_write_time(path);
  std::filesystem::last_write_time(path, when + std::chrono::hours(1));

  const ScanProgress progress = fixture.scanner->Scan(fixture.tree.root());

  REQUIRE(fixture.reads() == 1);
  REQUIRE(progress.updated == 1);
  REQUIRE(fixture.library.TrackCount() == 1);
}

TEST_CASE("a file that is gone loses its row", "[scanner]") {
  Fixture fixture;
  fixture.tree.Write("a.flac", "aaaa");
  fixture.tree.Write("b.flac", "bbbb");
  fixture.scanner->Scan(fixture.tree.root());

  std::filesystem::remove(fixture.tree.root() / "a.flac");
  const ScanProgress progress = fixture.scanner->Scan(fixture.tree.root());

  REQUIRE(progress.removed == 1);
  REQUIRE(fixture.library.TrackCount() == 1);
  REQUIRE(fixture.library.ListTracks()[0].title == "b (title)");
}

TEST_CASE("scanning one folder does not touch another folder's rows", "[scanner]") {
  // The bug this guards against empties the library: every row not seen by this
  // walk looks missing, and most of them are simply somewhere else.
  Fixture fixture;
  fixture.tree.Write("a.flac", "aaaa");

  Track elsewhere;
  elsewhere.path = "/somewhere/else/x.flac";
  elsewhere.title = "Elsewhere";
  fixture.library.Upsert(elsewhere);

  // A sibling folder whose name starts with the scanned root's name, which is
  // what a prefix comparison gets wrong if it forgets the separator.
  Track sibling;
  sibling.path = fixture.tree.root().string() + " Backup/y.flac";
  sibling.title = "Sibling";
  fixture.library.Upsert(sibling);

  const ScanProgress progress = fixture.scanner->Scan(fixture.tree.root());

  REQUIRE(progress.removed == 0);
  REQUIRE(fixture.library.TrackCount() == 3);
  REQUIRE(fixture.library.FindByPath("/somewhere/else/x.flac").has_value());
  REQUIRE(fixture.library.FindByPath(sibling.path).has_value());
}

TEST_CASE("remove_missing off leaves the rows alone", "[scanner]") {
  ScanOptions options;
  options.remove_missing = false;
  Fixture fixture(options);
  fixture.tree.Write("a.flac", "aaaa");
  fixture.scanner->Scan(fixture.tree.root());

  std::filesystem::remove(fixture.tree.root() / "a.flac");
  const ScanProgress progress = fixture.scanner->Scan(fixture.tree.root());

  REQUIRE(progress.removed == 0);
  REQUIRE(fixture.library.TrackCount() == 1);
}

TEST_CASE("everything that is not audio is skipped without being opened", "[scanner]") {
  Fixture fixture;
  fixture.tree.Write("a.flac", "aaaa");
  fixture.tree.Write("cover.jpg", "not audio");
  fixture.tree.Write("notes.txt", "not audio either");
  fixture.tree.Write("desktop.ini", "windows");
  fixture.tree.Write("no-extension", "nothing");

  const ScanProgress progress = fixture.scanner->Scan(fixture.tree.root());

  REQUIRE(progress.files_seen == 1);
  REQUIRE(fixture.reads() == 1);
  REQUIRE(fixture.library.TrackCount() == 1);
}

TEST_CASE("the extension match ignores case", "[scanner]") {
  // Music copied off a CD by an older program is full of .FLAC and .MP3.
  Fixture fixture;
  fixture.tree.Write("a.FLAC", "aaaa");
  fixture.tree.Write("b.Mp3", "bbbb");

  const ScanProgress progress = fixture.scanner->Scan(fixture.tree.root());

  REQUIRE(progress.files_seen == 2);
  REQUIRE(fixture.library.TrackCount() == 2);
}

TEST_CASE("a file the reader cannot read is counted, not indexed", "[scanner]") {
  Fixture fixture;
  fixture.tree.Write("a.flac", "aaaa");
  fixture.tree.Write("broken.flac", "truncated");

  const ScanProgress progress = fixture.scanner->Scan(fixture.tree.root());

  REQUIRE(progress.files_seen == 2);
  REQUIRE(progress.files_read == 2);
  REQUIRE(progress.failed == 1);
  REQUIRE(progress.added == 1);
  REQUIRE(fixture.library.TrackCount() == 1);

  // And it is tried again next time rather than remembered as done: there is no
  // row, so there is no stamp to skip it by.
  fixture.ResetReads();
  fixture.scanner->Scan(fixture.tree.root());
  REQUIRE(fixture.reads() == 1);
}

TEST_CASE("a file with no title tag is still findable by its name", "[scanner]") {
  Fixture fixture;
  fixture.tree.Write("untitled.flac", "aaaa");

  fixture.scanner->Scan(fixture.tree.root());

  const std::vector<Track> tracks = fixture.library.ListTracks();
  REQUIRE(tracks.size() == 1);
  REQUIRE(tracks[0].title == "untitled");
  REQUIRE(fixture.library.Search("untitled").size() == 1);
}

TEST_CASE("a missing album artist falls back to the artist", "[scanner]") {
  // Otherwise every album on a normal, non-compilation library groups under the
  // empty string and the album list is one enormous nameless album.
  Fixture fixture;
  fixture.tree.Write("a.flac", "aaaa");

  fixture.scanner->Scan(fixture.tree.root());

  const std::vector<AlbumSummary> albums = fixture.library.ListAlbums();
  REQUIRE(albums.size() == 1);
  REQUIRE(albums[0].album_artist == "Artist");
}

TEST_CASE("progress is reported during the scan and once at the end", "[scanner]") {
  ScanOptions options;
  options.batch_size = 2;
  Fixture fixture(options);
  for (int i = 0; i < 9; ++i) {
    fixture.tree.Write("track" + std::to_string(i) + ".flac", std::string(4, 'x'));
  }

  std::vector<ScanProgress> reports;
  const ScanProgress progress = fixture.scanner->Scan(
      fixture.tree.root(),
      [&reports](const ScanProgress& update) { reports.push_back(update); });

  REQUIRE_FALSE(reports.empty());
  REQUIRE(reports.back().done);
  REQUIRE(reports.back().added == 9);
  REQUIRE(progress.added == 9);
  REQUIRE(fixture.library.TrackCount() == 9);
}

TEST_CASE("a single worker gives the same answer as several", "[scanner]") {
  // The pool is the part that could plausibly lose or duplicate a result, so the
  // parallel answer is compared against the serial one.
  ScanOptions serial;
  serial.worker_count = 1;
  Fixture one(serial);

  ScanOptions parallel;
  parallel.worker_count = 4;
  Fixture many(parallel);

  for (int i = 0; i < 40; ++i) {
    const std::string name = "t" + std::to_string(i) + ".flac";
    one.tree.Write(name, std::string(static_cast<std::size_t>(i) + 1, 'x'));
    many.tree.Write(name, std::string(static_cast<std::size_t>(i) + 1, 'x'));
  }

  const ScanProgress first = one.scanner->Scan(one.tree.root());
  const ScanProgress second = many.scanner->Scan(many.tree.root());

  REQUIRE(first.added == 40);
  REQUIRE(second.added == 40);
  REQUIRE(one.library.TrackCount() == 40);
  REQUIRE(many.library.TrackCount() == 40);
  REQUIRE(one.reads() == 40);
  REQUIRE(many.reads() == 40);
}

TEST_CASE("a cancelled scan removes nothing", "[scanner]") {
  // Cancel means the walk did not finish, so "not seen" does not mean "not
  // there". Acting on it would delete most of the library.
  Fixture fixture;
  fixture.tree.Write("a.flac", "aaaa");
  fixture.tree.Write("b.flac", "bbbb");
  fixture.scanner->Scan(fixture.tree.root());
  std::filesystem::remove(fixture.tree.root() / "a.flac");

  // Cancelled from the progress callback, which arrives on this thread during
  // the scan. That is also how the shell will do it: the library thread reports,
  // and whoever is listening can say stop.
  Scanner* scanner = fixture.scanner.get();
  const ScanProgress progress = fixture.scanner->Scan(
      fixture.tree.root(), [scanner](const ScanProgress&) { scanner->Cancel(); });

  REQUIRE(progress.cancelled);
  REQUIRE(progress.done);
  REQUIRE(progress.removed == 0);
  REQUIRE(fixture.library.TrackCount() == 2);

  // Cancel applies to the scan in flight, not to the scanner: the next one runs.
  const ScanProgress after = fixture.scanner->Scan(fixture.tree.root());
  REQUIRE_FALSE(after.cancelled);
  REQUIRE(after.removed == 1);
}

TEST_CASE("an album's tracks share one stored cover", "[scanner][covers]") {
  // The reason covers are keyed by content: twelve tracks of an album embed the
  // same JPEG, and an index that stored it twelve times would be mostly
  // duplicated pictures.
  Fixture fixture;
  fixture.tree.Write("art1.flac", "aaaa");
  fixture.tree.Write("art2.flac", "bbbb");
  fixture.tree.Write("art3.flac", "cccc");
  fixture.tree.Write("other.flac", "dddd");
  fixture.tree.Write("plain.flac", "eeee");

  const ScanProgress progress = fixture.scanner->Scan(fixture.tree.root());

  REQUIRE(progress.added == 5);
  REQUIRE(progress.covers == 2);  // one shared by three tracks, one on its own
  REQUIRE(fixture.library.CoverCount() == 2);

  const std::optional<Track> first =
      fixture.library.FindByPath(ToUtf8(fixture.tree.root() / "art1.flac"));
  const std::optional<Track> second =
      fixture.library.FindByPath(ToUtf8(fixture.tree.root() / "art2.flac"));
  const std::optional<Track> plain =
      fixture.library.FindByPath(ToUtf8(fixture.tree.root() / "plain.flac"));
  REQUIRE(first.has_value());
  REQUIRE(second.has_value());
  REQUIRE(plain.has_value());
  REQUIRE_FALSE(first->cover_hash.empty());
  REQUIRE(first->cover_hash == second->cover_hash);
  REQUIRE(plain->cover_hash.empty());

  const std::optional<Cover> stored = fixture.library.GetCover(first->cover_hash);
  REQUIRE(stored.has_value());
  REQUIRE(stored->mime == "image/png");
  REQUIRE(stored->bytes == std::vector<std::uint8_t>{1, 2, 3, 4, 5, 6, 7, 8});
}

TEST_CASE("a rescan does not re-read artwork it already has", "[scanner][covers]") {
  Fixture fixture;
  fixture.tree.Write("art1.flac", "aaaa");
  fixture.tree.Write("art2.flac", "bbbb");
  fixture.scanner->Scan(fixture.tree.root());
  REQUIRE(fixture.library.CoverCount() == 1);

  const ScanProgress again = fixture.scanner->Scan(fixture.tree.root());
  REQUIRE(again.covers == 0);
  REQUIRE(fixture.library.CoverCount() == 1);
}

TEST_CASE("read_covers off skips the artwork entirely", "[scanner][covers]") {
  ScanOptions options;
  options.read_covers = false;
  Fixture fixture(options);
  fixture.tree.Write("art1.flac", "aaaa");

  const ScanProgress progress = fixture.scanner->Scan(fixture.tree.root());

  REQUIRE(progress.added == 1);
  REQUIRE(progress.covers == 0);
  REQUIRE(fixture.library.CoverCount() == 0);
  // Not just "no cover stored": the reader was told not to look, which is where
  // the time actually goes.
  REQUIRE(fixture.reader->cover_reads.load() == 0);
}

TEST_CASE("a deleted album takes its cover with it", "[scanner][covers]") {
  Fixture fixture;
  fixture.tree.Write("art1.flac", "aaaa");
  fixture.tree.Write("other.flac", "bbbb");
  fixture.scanner->Scan(fixture.tree.root());
  REQUIRE(fixture.library.CoverCount() == 2);

  std::filesystem::remove(fixture.tree.root() / "art1.flac");
  const ScanProgress progress = fixture.scanner->Scan(fixture.tree.root());

  REQUIRE(progress.removed == 1);
  REQUIRE(progress.pruned == 1);
  REQUIRE(fixture.library.CoverCount() == 1);
}

TEST_CASE("scanning something that is not a folder says so", "[scanner]") {
  Fixture fixture;
  const std::filesystem::path file = fixture.tree.Write("a.flac", "aaaa");

  REQUIRE_THROWS_AS(fixture.scanner->Scan(file), LibraryError);
  REQUIRE_THROWS_AS(fixture.scanner->Scan(fixture.tree.root() / "nope"), LibraryError);
}
