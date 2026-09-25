#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <bridge_generated.h>
#include <sonora/library/library.h>
#include <sonora/library/scanner.h>
#include <sonora/library/track.h>

#include "cef/timer.h"

namespace sonora::shell {

// Owns the index, the scanner and the thread a scan runs on, and turns the
// scan's progress into events on the bridge.
//
// The thread is the whole point. A scan of a real music folder is minutes of
// file reading, and the CEF UI thread is the thread the interface repaints on --
// so the two must never be the same thread, and no bridge call may wait for a
// scan to finish. What crosses between them is small: a request to start, and a
// status somebody reads.
//
// Queries are a different matter. library.listAlbums is a few milliseconds of
// SQLite and it is answered synchronously, on the UI thread, straight out of the
// index -- because Library takes its own lock, which is exactly the case its
// mutex exists for: a query from the UI thread against a batch being written by
// the scan thread.
class LibraryHost {
 public:
  // The key the scanned folder is remembered under, so a restart does not have
  // to be told again.
  static constexpr char kRootSetting[] = "library.root";

  LibraryHost();
  ~LibraryHost();

  LibraryHost(const LibraryHost&) = delete;
  LibraryHost& operator=(const LibraryHost&) = delete;

  // Opens or creates the index. False when it cannot be opened at all -- a
  // read-only disk, a file written by a newer Sonora -- and then the library
  // capability is switched off, which is the same degraded path a machine with
  // no sound card takes for the player.
  bool Start(const std::filesystem::path& database_path);

  // Cancels any scan, joins the thread, closes the index. Idempotent.
  void Stop();

  [[nodiscard]] bool started() const { return library_ != nullptr; }
  [[nodiscard]] std::string description() const;

  // Starts a scan of `root`, or of the remembered folder when it is empty.
  // False when a scan is already running or there is no folder to scan.
  bool StartScan(const std::filesystem::path& root);

  // Begins emitting library.status. Called once the event channel exists.
  void StartStatusEvents(bridge::EventSink& sink);

  // The whole status, in one place, so library.getStatus and the library.status
  // event cannot drift apart.
  [[nodiscard]] bridge::LibraryStatus Status() const;

  [[nodiscard]] library::Library* index() const { return library_.get(); }

  // Where the page should fetch a cover, or empty for a track that has none.
  // Built here so that no part of the page ever assembles a URL out of a hash.
  [[nodiscard]] static std::string ArtUrl(const std::string& cover_hash);

  // Called from the CEF IO thread by the scheme handler. Safe because Library is
  // safe; see the class comment.
  [[nodiscard]] std::optional<library::Cover> Cover(const std::string& cover_hash) const;

  // Fills a generated record from an index row. Here rather than in handlers.cpp
  // because the queue needs it too.
  [[nodiscard]] static bridge::LibraryTrack ToBridge(const library::Track& track);
  [[nodiscard]] static bridge::LibraryAlbum ToBridge(const library::AlbumSummary& album);

 private:
  void RunScan(std::filesystem::path root);

  std::unique_ptr<library::Library> library_;
  std::unique_ptr<library::Scanner> scanner_;

  std::thread thread_;
  std::atomic<bool> scanning_{false};
  // Scans finished since startup. Not a statistic: it is what makes a scan that
  // changed nothing still visible to the page. See StartStatusEvents.
  std::atomic<std::uint64_t> scans_{0};

  mutable std::mutex mutex_;  // root_, progress_, last_error_
  std::string root_;
  library::ScanProgress progress_;
  std::string last_error_;

  CefRefPtr<ShellTimer> status_timer_;
  // UI thread only: what the page was last told, so a library that is not doing
  // anything does not post an event four times a second forever.
  std::string last_emitted_;
};

}  // namespace sonora::shell
