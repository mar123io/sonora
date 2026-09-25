#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sonora::library {

// One file in the library, as the index knows it.
//
// Paths are UTF-8 here and everywhere above the platform layer. On Windows a
// std::filesystem::path is UTF-16, so the conversion happens once, at the edge
// of this target, rather than being rediscovered by every caller.
struct Track {
  std::int64_t id = 0;
  std::string path;
  std::string title;
  std::string artist;
  std::string album;
  std::string album_artist;
  int track_number = 0;
  int disc_number = 0;
  int year = 0;
  std::int64_t duration_ms = 0;

  // Identifies the cover this file carries, or empty when it carries none.
  //
  // A hash rather than the image: every track of an album embeds the same
  // picture, so twelve tracks share one row in `covers` and one URL, which the
  // browser then caches once. It is also what makes the cover addressable at
  // all -- a page cannot be handed a megabyte of JPEG through the bridge.
  std::string cover_hash;

  // What the incremental scan compares against. Two files with the same
  // modification time and the same size are taken to be unchanged -- which is
  // wrong for a file edited within the filesystem's timestamp resolution and
  // restored to the same length, and right for everything anyone actually
  // does to a music file.
  std::int64_t mtime_ns = 0;
  std::int64_t size_bytes = 0;
};

// A row of the album list: not a table, a grouping. See the comment on
// Library for why there is no albums table.
struct AlbumSummary {
  std::string album;
  std::string album_artist;
  int year = 0;
  int track_count = 0;
  std::int64_t duration_ms = 0;
  // Of any track of the album that has one. An album where only track 7 was
  // tagged with the artwork still has a cover in the list.
  std::string cover_hash;
};

// One picture, as it came out of a tag and as it goes back out to the page.
//
// The mime type is sniffed from the bytes rather than taken from the tag: the
// tag is written by whatever program touched the file last, the response
// carries X-Content-Type-Options: nosniff, and a picture served as the wrong
// type is a broken image with no explanation.
struct Cover {
  std::string mime;
  std::vector<std::uint8_t> bytes;
};

}  // namespace sonora::library
