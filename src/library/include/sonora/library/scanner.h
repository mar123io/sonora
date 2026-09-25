#pragma once

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <sonora/library/library.h>
#include <sonora/library/tag_reader.h>

namespace sonora::library {

struct ScanOptions {
  // Lowercase, with the dot. Only what this build can actually play: indexing a
  // .m4a that the decoder will refuse produces a row that fails when somebody
  // clicks it, which is worse than not having it. The list grows when the
  // decoders do.
  std::vector<std::string> extensions = {".flac", ".mp3", ".wav"};

  // 0 asks for a sensible number. Tag reading is mostly waiting for the disk
  // with a little parsing on top, so it does scale past one thread -- and it
  // stops scaling at about four, because after that the threads are queued
  // behind the same device. The default is deliberately not
  // hardware_concurrency(): a scan is a background chore and has no business
  // taking every core on the machine.
  int worker_count = 0;

  // Rows whose file is gone are deleted. Only under the root being scanned --
  // scanning one folder must never empty another.
  bool remove_missing = true;

  // Read and store embedded artwork. Off makes a scan measurably faster and the
  // interface measurably worse, so it is on; it exists as a switch because a
  // library on a slow network share is a different trade-off.
  bool read_covers = true;

  // How many tracks are written per transaction.
  std::size_t batch_size = 256;
};

struct ScanProgress {
  int files_seen = 0;  // audio files found by the walk
  int files_read = 0;  // ...of which the tags were actually read
  int added = 0;
  int updated = 0;
  int removed = 0;
  int failed = 0;  // could not be read; not indexed, not fatal
  int covers = 0;  // distinct pictures stored; far fewer than tracks
  int pruned = 0;  // covers dropped because nothing refers to them any more
  bool cancelled = false;
  bool done = false;
};

// Walks a folder and brings the index in step with it.
//
// The interesting number is files_read, not files_seen. A second scan of an
// unchanged folder reads no tags at all: it walks the tree, compares each file's
// modification time and size against what the index already holds, and stops
// there. That is what makes it reasonable to scan at startup -- the cost is a
// directory walk, not a re-read of every file in the music folder.
//
// Threading: Scan() blocks the thread it is called on and uses a small pool for
// the tag reads. It is meant to be called from a thread of its own -- in the
// shell that is the library thread, never the CEF UI thread. Progress callbacks
// arrive on the calling thread, which is what lets the caller marshal them
// wherever they need to go without a second lock.
class Scanner {
 public:
  Scanner(Library& library, TagReaderPtr reader, ScanOptions options = {});
  ~Scanner();

  Scanner(const Scanner&) = delete;
  Scanner& operator=(const Scanner&) = delete;

  // `on_progress` may be empty. It is called from time to time during the scan
  // and once at the end, with done set.
  ScanProgress Scan(const std::filesystem::path& root,
                    const std::function<void(const ScanProgress&)>& on_progress = {});

  // Asks the scan in flight to stop. Safe from any thread, including from a
  // progress callback. The scan returns what it had finished, with cancelled set,
  // and removes nothing -- a walk that did not finish cannot tell a missing file
  // from an unvisited one. Work already written stays written: that is the point
  // of the index being resumable rather than transactional.
  //
  // Each Scan() starts uncancelled, so calling this before one does nothing.
  void Cancel() noexcept;

 private:
  Library& library_;
  TagReaderPtr reader_;
  ScanOptions options_;
  std::atomic<bool> cancelled_{false};
};

}  // namespace sonora::library
