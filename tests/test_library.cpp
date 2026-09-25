#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <sonora/library/library.h>

using namespace sonora::library;

namespace {

// ":memory:" throughout: these tests are about SQL, not about files, and an
// in-memory index makes the whole file run in milliseconds with nothing to clean
// up afterwards.
[[nodiscard]] Library MakeLibrary() {
  return Library(":memory:");
}

[[nodiscard]] Track MakeTrack(const std::string& path,
                              const std::string& title,
                              const std::string& artist,
                              const std::string& album,
                              int track_number = 1) {
  Track track;
  track.path = path;
  track.title = title;
  track.artist = artist;
  track.album = album;
  track.album_artist = artist;
  track.track_number = track_number;
  track.duration_ms = 200000;
  track.mtime_ns = 1000;
  track.size_bytes = 4096;
  return track;
}

[[nodiscard]] std::vector<std::string> Titles(const std::vector<Track>& tracks) {
  std::vector<std::string> titles;
  titles.reserve(tracks.size());
  for (const Track& track : tracks) {
    titles.push_back(track.title);
  }
  return titles;
}

}  // namespace

TEST_CASE("a fresh index is at the current schema version", "[library]") {
  Library library = MakeLibrary();
  REQUIRE(library.TrackCount() == 0);
  REQUIRE(library.ListTracks().empty());
  REQUIRE(library.ListAlbums().empty());
}

TEST_CASE("a track round-trips through the index", "[library]") {
  Library library = MakeLibrary();
  Track track = MakeTrack("/music/a.flac", "Vita spericolata", "Vasco Rossi", "Bollicine", 3);
  track.year = 1983;
  track.disc_number = 1;

  const std::int64_t id = library.Upsert(track);
  REQUIRE(id > 0);

  const std::optional<Track> stored = library.Get(id);
  REQUIRE(stored.has_value());
  REQUIRE(stored->path == track.path);
  REQUIRE(stored->title == track.title);
  REQUIRE(stored->artist == track.artist);
  REQUIRE(stored->album == track.album);
  REQUIRE(stored->track_number == 3);
  REQUIRE(stored->year == 1983);
  REQUIRE(stored->duration_ms == 200000);
  REQUIRE(stored->mtime_ns == 1000);
  REQUIRE(stored->size_bytes == 4096);

  REQUIRE(library.FindByPath("/music/a.flac").has_value());
  REQUIRE_FALSE(library.FindByPath("/music/nothing.flac").has_value());
  REQUIRE_FALSE(library.Get(id + 100).has_value());
}

TEST_CASE("the path is the identity: the same file twice is one row", "[library]") {
  // This is what makes a rescan safe. Without it, every scan of an unchanged
  // folder would be a full set of duplicates.
  Library library = MakeLibrary();
  const std::int64_t first = library.Upsert(MakeTrack("/music/a.flac", "Old", "A", "X"));

  Track retagged = MakeTrack("/music/a.flac", "New", "A", "X");
  retagged.mtime_ns = 2000;
  const std::int64_t second = library.Upsert(retagged);

  REQUIRE(first == second);
  REQUIRE(library.TrackCount() == 1);
  REQUIRE(library.Get(first)->title == "New");
  REQUIRE(library.Get(first)->mtime_ns == 2000);
}

TEST_CASE("a batch is one transaction and counts the same as one-by-one", "[library]") {
  Library library = MakeLibrary();
  library.UpsertBatch({MakeTrack("/music/a.flac", "A", "Artist", "Album", 1),
                       MakeTrack("/music/b.flac", "B", "Artist", "Album", 2),
                       MakeTrack("/music/c.flac", "C", "Artist", "Album", 3)});

  REQUIRE(library.TrackCount() == 3);
  REQUIRE(Titles(library.ListTracks()) == std::vector<std::string>{"A", "B", "C"});

  // An empty batch is not an error; the scanner hands one over whenever nothing
  // changed, which is the common case.
  library.UpsertBatch({});
  REQUIRE(library.TrackCount() == 3);
}

TEST_CASE("tracks come back in album order, not insertion order", "[library]") {
  Library library = MakeLibrary();
  library.UpsertBatch({MakeTrack("/m/3.flac", "Third", "Artist", "Album", 3),
                       MakeTrack("/m/1.flac", "First", "Artist", "Album", 1),
                       MakeTrack("/m/2.flac", "Second", "Artist", "Album", 2)});

  REQUIRE(Titles(library.ListTracks()) == std::vector<std::string>{"First", "Second", "Third"});

  // A negative limit means everything, which is what the default argument says.
  REQUIRE(library.ListTracks(2).size() == 2);
  REQUIRE(Titles(library.ListTracks(2, 1)) == std::vector<std::string>{"Second", "Third"});
}

TEST_CASE("albums are a grouping, with the earliest year", "[library]") {
  Library library = MakeLibrary();
  Track first = MakeTrack("/m/1.flac", "One", "Artist", "Album", 1);
  first.year = 1994;
  Track second = MakeTrack("/m/2.flac", "Two", "Artist", "Album", 2);
  second.year = 2011;  // a remaster, retagged; the album is still from 1994
  library.UpsertBatch({first, second, MakeTrack("/m/3.flac", "Other", "Other", "Else")});

  const std::vector<AlbumSummary> albums = library.ListAlbums();
  REQUIRE(albums.size() == 2);

  const AlbumSummary& album = albums[0];  // ordered by album artist: Artist, Other
  REQUIRE(album.album == "Album");
  REQUIRE(album.album_artist == "Artist");
  REQUIRE(album.track_count == 2);
  REQUIRE(album.year == 1994);
  REQUIRE(album.duration_ms == 400000);

  REQUIRE(library.ListArtists() == std::vector<std::string>{"Artist", "Other"});
  REQUIRE(library.AlbumTracks("Album", "Artist").size() == 2);
  REQUIRE(library.AlbumTracks("Album", "Nobody").empty());
}

TEST_CASE("search finds a track by any of its text", "[library][search]") {
  Library library = MakeLibrary();
  library.UpsertBatch({MakeTrack("/m/1.flac", "Paranoid Android", "Radiohead", "OK Computer"),
                       MakeTrack("/m/2.flac", "Let Down", "Radiohead", "OK Computer", 5),
                       MakeTrack("/m/3.flac", "Teardrop", "Massive Attack", "Mezzanine")});

  REQUIRE(library.Search("paranoid").size() == 1);
  REQUIRE(library.Search("radiohead").size() == 2);
  REQUIRE(library.Search("mezzanine").size() == 1);
  REQUIRE(library.Search("nothing like this").empty());

  // Two words are an AND, not a phrase: "computer down" is how somebody
  // remembers a track, and it should still find it.
  REQUIRE(library.Search("computer down").size() == 1);
}

TEST_CASE("the last word of a search is a prefix", "[library][search]") {
  // What makes the list narrow while somebody is still typing.
  Library library = MakeLibrary();
  library.Upsert(MakeTrack("/m/1.flac", "Lucio", "Battisti", "Anima latina"));

  REQUIRE(library.Search("batt").size() == 1);
  REQUIRE(library.Search("b").size() == 1);
  REQUIRE(library.Search("battx").empty());
  // Only the last word, which is the one still being typed. An earlier word has
  // to match whole, or every short word in the box would match half the library.
  REQUIRE(library.Search("lucio batt").size() == 1);
  REQUIRE(library.Search("batt lucio").empty());
}

TEST_CASE("search ignores diacritics in both directions", "[library][search]") {
  // An Italian library with an English keyboard, which is the normal case.
  Library library = MakeLibrary();
  library.UpsertBatch({MakeTrack("/m/1.flac", "Così celeste", "Zucchero", "Miserere"),
                       MakeTrack("/m/2.flac", "Jóga", "Björk", "Homogenic")});

  REQUIRE(library.Search("cosi").size() == 1);
  REQUIRE(library.Search("così").size() == 1);
  REQUIRE(library.Search("bjork").size() == 1);
  REQUIRE(library.Search("björk").size() == 1);
}

TEST_CASE("whatever is in the search box is text, never syntax", "[library][search]") {
  // FTS5 has a grammar of its own -- quotes, colons, NEAR, AND -- and a search
  // box that passes its contents through unchanged is a syntax error waiting for
  // the first apostrophe. None of these may throw.
  Library library = MakeLibrary();
  library.UpsertBatch({MakeTrack("/m/1.flac", "Livin' on a Prayer", "Bon Jovi", "Slippery"),
                       MakeTrack("/m/2.flac", "AND NEAR OR", "Weird", "Operators")});

  REQUIRE(library.Search("livin").size() == 1);
  REQUIRE(library.Search("livin'").size() == 1);
  REQUIRE_NOTHROW(library.Search("\""));
  REQUIRE_NOTHROW(library.Search("\"unbalanced"));
  REQUIRE_NOTHROW(library.Search("title:bon"));
  REQUIRE_NOTHROW(library.Search("a NEAR/2 b"));
  REQUIRE_NOTHROW(library.Search("*"));
  REQUIRE_NOTHROW(library.Search("^"));
  REQUIRE_NOTHROW(library.Search("-"));

  // A column filter is not honoured, because it is not syntax any more: it is a
  // search for those two words.
  REQUIRE(library.Search("title:bon").empty());
  // ...and an operator typed as text finds the track that is called that.
  REQUIRE(library.Search("near").size() == 1);
}

TEST_CASE("an empty search is not a search for everything", "[library][search]") {
  Library library = MakeLibrary();
  library.Upsert(MakeTrack("/m/1.flac", "A", "B", "C"));

  REQUIRE(library.Search("").empty());
  REQUIRE(library.Search("   ").empty());
  REQUIRE(library.Search("''").empty());
}

TEST_CASE("the match expression turns words into phrases", "[library][search]") {
  // The unit behind the tests above, checked directly so that a failure says
  // which half is wrong.
  REQUIRE(detail::MakeMatchQuery("").empty());
  REQUIRE(detail::MakeMatchQuery("  ").empty());
  REQUIRE(detail::MakeMatchQuery("one") == "\"one\"*");
  REQUIRE(detail::MakeMatchQuery("one two") == "\"one\" \"two\"*");
  REQUIRE(detail::MakeMatchQuery("  one   two  ") == "\"one\" \"two\"*");
  REQUIRE(detail::MakeMatchQuery("NEAR") == "\"NEAR\"*");
  REQUIRE(detail::MakeMatchQuery("a\"b") == "\"a\"\"b\"*");
  REQUIRE(detail::MakeMatchQuery("\"").empty());
  REQUIRE(detail::MakeMatchQuery("ok -") == "\"ok\"*");
}

TEST_CASE("removing a track removes it from the search index too", "[library][search]") {
  // The FTS table has its own copy of the text, kept in step by triggers. If the
  // delete trigger is wrong, the row stays searchable forever and clicking the
  // result opens a file that is not there.
  Library library = MakeLibrary();
  const std::int64_t id = library.Upsert(MakeTrack("/m/1.flac", "Ghost", "Artist", "Album"));
  REQUIRE(library.Search("ghost").size() == 1);

  library.Remove(id);
  REQUIRE(library.Search("ghost").empty());
  REQUIRE(library.TrackCount() == 0);
}

TEST_CASE("retagging a track changes what it is found by", "[library][search]") {
  // The update trigger has to delete the old text as well as insert the new. A
  // missing delete leaves the old title searchable, which looks exactly like a
  // stale cache and is one.
  Library library = MakeLibrary();
  library.Upsert(MakeTrack("/m/1.flac", "Wrong Title", "Artist", "Album"));
  REQUIRE(library.Search("wrong").size() == 1);

  library.Upsert(MakeTrack("/m/1.flac", "Right Title", "Artist", "Album"));
  REQUIRE(library.Search("wrong").empty());
  REQUIRE(library.Search("right").size() == 1);
}

TEST_CASE("stamps are what the incremental scan compares against", "[library]") {
  Library library = MakeLibrary();
  Track track = MakeTrack("/m/1.flac", "A", "B", "C");
  track.mtime_ns = 12345;
  track.size_bytes = 999;
  const std::int64_t id = library.Upsert(track);

  const auto stamps = library.Stamps();
  REQUIRE(stamps.size() == 1);
  const auto found = stamps.find("/m/1.flac");
  REQUIRE(found != stamps.end());
  REQUIRE(found->second.id == id);
  REQUIRE(found->second.mtime_ns == 12345);
  REQUIRE(found->second.size_bytes == 999);
}

TEST_CASE("several tracks come back in the order they were asked for", "[library]") {
  // What player.enqueue is built on: an album is one query, and it plays in the
  // order the page listed it rather than in the order the table holds it.
  Library library = MakeLibrary();
  library.UpsertBatch({MakeTrack("/m/1.flac", "A", "X", "Y", 1),
                       MakeTrack("/m/2.flac", "B", "X", "Y", 2),
                       MakeTrack("/m/3.flac", "C", "X", "Y", 3)});
  const auto stamps = library.Stamps();
  const std::int64_t first = stamps.at("/m/1.flac").id;
  const std::int64_t second = stamps.at("/m/2.flac").id;
  const std::int64_t third = stamps.at("/m/3.flac").id;

  REQUIRE(Titles(library.GetMany({third, first, second})) ==
          std::vector<std::string>{"C", "A", "B"});
  // An id that is not there is skipped, not an error and not a gap in the list.
  REQUIRE(Titles(library.GetMany({first, 9999, second})) == std::vector<std::string>{"A", "B"});
  REQUIRE(library.GetMany({}).empty());
  REQUIRE(library.GetMany({9999}).empty());
}

TEST_CASE("the counts the status event reports", "[library]") {
  Library library = MakeLibrary();
  library.UpsertBatch({MakeTrack("/m/1.flac", "A", "One", "First"),
                       MakeTrack("/m/2.flac", "B", "One", "Second"),
                       MakeTrack("/m/3.flac", "C", "Two", "Third")});

  REQUIRE(library.TrackCount() == 3);
  REQUIRE(library.AlbumCount() == 3);
  REQUIRE(library.ArtistCount() == 2);
}

TEST_CASE("everything by one artist, grouped by album", "[library]") {
  Library library = MakeLibrary();
  library.UpsertBatch({MakeTrack("/m/1.flac", "Later", "Artist", "Second", 1),
                       MakeTrack("/m/2.flac", "Earlier", "Artist", "First", 1),
                       MakeTrack("/m/3.flac", "Someone else", "Other", "Third", 1)});

  REQUIRE(Titles(library.ArtistTracks("Artist")) ==
          std::vector<std::string>{"Earlier", "Later"});
  REQUIRE(library.ArtistTracks("Nobody").empty());
}

TEST_CASE("a cover is stored once per picture, not once per track", "[library][covers]") {
  Library library = MakeLibrary();
  Cover cover;
  cover.mime = "image/jpeg";
  cover.bytes = {0xFF, 0xD8, 0xFF, 0x01, 0x02};
  const std::string hash = CoverHash(cover.bytes);

  REQUIRE_FALSE(library.HasCover(hash));
  library.PutCover(hash, cover);
  REQUIRE(library.HasCover(hash));
  REQUIRE(library.CoverCount() == 1);

  // Twice is not twice: the album's other eleven tracks hand over the same
  // picture and the second insert is ignored.
  library.PutCover(hash, cover);
  REQUIRE(library.CoverCount() == 1);

  const std::optional<Cover> stored = library.GetCover(hash);
  REQUIRE(stored.has_value());
  REQUIRE(stored->mime == "image/jpeg");
  REQUIRE(stored->bytes == cover.bytes);

  REQUIRE_FALSE(library.GetCover("nothing").has_value());
  REQUIRE_FALSE(library.HasCover(""));
  REQUIRE_FALSE(library.GetCover("").has_value());
}

TEST_CASE("a cover survives bytes that are not text", "[library][covers]") {
  // A JPEG contains zero bytes, and a zero byte is the end of a C string. If
  // these ever went through the text binding, a cover would come back truncated
  // at its first zero -- which for a JPEG is within the first few bytes.
  Library library = MakeLibrary();
  Cover cover;
  cover.mime = "image/png";
  cover.bytes = {0x89, 'P', 'N', 'G', 0x00, 0x00, 0x00, 0x0D, 0xFF, 0x00, 0x42};
  const std::string hash = CoverHash(cover.bytes);
  library.PutCover(hash, cover);

  const std::optional<Cover> stored = library.GetCover(hash);
  REQUIRE(stored.has_value());
  REQUIRE(stored->bytes.size() == cover.bytes.size());
  REQUIRE(stored->bytes == cover.bytes);
}

TEST_CASE("the hash is of the content, and of its length", "[library][covers]") {
  const std::vector<std::uint8_t> one = {1, 2, 3};
  const std::vector<std::uint8_t> same = {1, 2, 3};
  const std::vector<std::uint8_t> different = {1, 2, 4};
  const std::vector<std::uint8_t> longer = {1, 2, 3, 0};

  REQUIRE(CoverHash(one) == CoverHash(same));
  REQUIRE(CoverHash(one) != CoverHash(different));
  REQUIRE(CoverHash(one) != CoverHash(longer));
  REQUIRE(CoverHash(one).size() == 24);
  REQUIRE(CoverHash({}) != CoverHash(one));
  // Hex only: it ends up in a URL.
  REQUIRE(CoverHash(one).find_first_not_of("0123456789abcdef") == std::string::npos);
}

TEST_CASE("an album takes a cover from whichever track has one", "[library][covers]") {
  // Albums where only one track was tagged with the artwork are common, and an
  // album list with a hole in it looks like a bug in the list.
  Library library = MakeLibrary();
  Track first = MakeTrack("/m/1.flac", "A", "Artist", "Album", 1);
  Track second = MakeTrack("/m/2.flac", "B", "Artist", "Album", 2);
  second.cover_hash = "abc123";
  library.UpsertBatch({first, second});

  const std::vector<AlbumSummary> albums = library.ListAlbums();
  REQUIRE(albums.size() == 1);
  REQUIRE(albums[0].cover_hash == "abc123");

  REQUIRE(library.FindByPath("/m/1.flac")->cover_hash.empty());
  REQUIRE(library.FindByPath("/m/2.flac")->cover_hash == "abc123");
}

TEST_CASE("pruning drops the pictures nothing points at", "[library][covers]") {
  Library library = MakeLibrary();
  Cover kept;
  kept.mime = "image/png";
  kept.bytes = {1, 2, 3};
  Cover orphan;
  orphan.mime = "image/png";
  orphan.bytes = {4, 5, 6};
  library.PutCover("kept", kept);
  library.PutCover("orphan", orphan);

  Track track = MakeTrack("/m/1.flac", "A", "X", "Y");
  track.cover_hash = "kept";
  library.Upsert(track);

  REQUIRE(library.PruneCovers() == 1);
  REQUIRE(library.HasCover("kept"));
  REQUIRE_FALSE(library.HasCover("orphan"));
  // Idempotent: nothing is orphaned the second time.
  REQUIRE(library.PruneCovers() == 0);
}

TEST_CASE("a setting survives, and an unknown one is absent rather than empty", "[library]") {
  Library library = MakeLibrary();

  REQUIRE_FALSE(library.GetSetting("library.root").has_value());
  library.SetSetting("library.root", "C:\\Music");
  REQUIRE(library.GetSetting("library.root") == "C:\\Music");

  // Written twice is written once: it is a key/value table, not a log.
  library.SetSetting("library.root", "D:\\Musica");
  REQUIRE(library.GetSetting("library.root") == "D:\\Musica");
  REQUIRE_FALSE(library.GetSetting("nothing.like.this").has_value());
}

TEST_CASE("an index on disk comes back at the current schema version", "[library]") {
  // The migration ladder, walked for real: a fresh file is created at version 1
  // and migrated to 2 in the same constructor, then reopened -- which is the
  // path every later schema change will take.
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("sonora-library-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".sqlite");
  std::error_code error;
  std::filesystem::remove(path, error);

  {
    Library library(path);
    Track track = MakeTrack("/m/1.flac", "Survivor", "Artist", "Album");
    track.cover_hash = "hash";
    library.Upsert(track);
    Cover cover;
    cover.mime = "image/png";
    cover.bytes = {1, 2, 3};
    library.PutCover("hash", cover);
    library.SetSetting("library.root", "/music");
  }
  {
    // Reopened: the migration must see version 2 and do nothing at all.
    Library library(path);
    REQUIRE(library.TrackCount() == 1);
    REQUIRE(library.CoverCount() == 1);
    REQUIRE(library.GetSetting("library.root") == "/music");
    REQUIRE(library.Search("survivor").size() == 1);
  }

  std::filesystem::remove(path, error);
}

TEST_CASE("a batch of removals is one transaction", "[library]") {
  Library library = MakeLibrary();
  library.UpsertBatch({MakeTrack("/m/1.flac", "A", "X", "Y"),
                       MakeTrack("/m/2.flac", "B", "X", "Y"),
                       MakeTrack("/m/3.flac", "C", "X", "Y")});

  const auto stamps = library.Stamps();
  library.RemoveBatch({stamps.at("/m/1.flac").id, stamps.at("/m/3.flac").id});

  REQUIRE(library.TrackCount() == 1);
  REQUIRE(library.FindByPath("/m/2.flac").has_value());
  library.RemoveBatch({});
  REQUIRE(library.TrackCount() == 1);
}
