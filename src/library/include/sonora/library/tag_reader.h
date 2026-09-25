#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include <sonora/library/track.h>

namespace sonora::library {

// What one file says about itself.
//
// Deliberately not a Track: a Track has an id and a size, which are facts about
// the index and the filesystem, not about the tags. Keeping them apart is what
// lets the scanner hand a file to a worker thread that has no idea a database
// exists.
struct TagData {
  std::string title;
  std::string artist;
  std::string album;
  std::string album_artist;
  int track_number = 0;
  int disc_number = 0;
  int year = 0;
  std::int64_t duration_ms = 0;

  // The embedded front cover, when the file has one. Absent is the common case
  // and not a failure.
  std::optional<Cover> cover;
};

// Reads tags. That is the whole interface, and the reason it exists.
//
// TagLib is a good library and a slow dependency: it opens the file, and for
// some formats it reads a long way into it. A test that wants to check that the
// scanner skips unchanged files must not pay for that, and -- more to the point
// -- must not need a real FLAC checked into the repository to have something to
// skip. So the scanner never names TagLib; it is handed one of these.
//
// The same seam is what keeps the macOS and Linux CI jobs building this target:
// there is exactly one file in the project that includes a TagLib header.
class TagReader {
 public:
  virtual ~TagReader() = default;

  // Must be callable from several threads at once, on different files. The
  // scanner does exactly that -- reading tags is the slow part of a scan and the
  // only part worth parallelising -- so an implementation that wants state has
  // to keep it on the stack. Hence const.
  //
  // std::nullopt when the file is not audio this reader understands, or cannot
  // be opened. Not an exception: a music folder contains cover.jpg, a Windows
  // desktop.ini and something a synchronisation client left half-written, and
  // none of those are errors worth unwinding a scan for.
  //
  // `with_cover` false skips reading the artwork, which is most of the bytes a
  // tag read touches. The scanner asks for it only when it is going to keep it.
  [[nodiscard]] virtual std::optional<TagData> Read(const std::filesystem::path& path,
                                                    bool with_cover) const = 0;

  // For the log line at the end of a scan.
  [[nodiscard]] virtual std::string description() const = 0;

 protected:
  TagReader() = default;
};

using TagReaderPtr = std::unique_ptr<TagReader>;

// The real one. Declared here, implemented in the one file that includes
// TagLib.
[[nodiscard]] TagReaderPtr MakeTagLibReader();

}  // namespace sonora::library
