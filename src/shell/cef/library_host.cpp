#include "cef/library_host.h"

#include <cstdio>
#include <utility>

#include "cef/scheme_handler.h"

namespace sonora::shell {
namespace {

// Sampled five times a second; the coalescer decides what the page actually
// gets. Same arrangement as the transport, and the same reason: sampling faster
// than delivery means the value that arrives is the newest one.
constexpr std::int64_t kStatusSampleMs = 200;

[[nodiscard]] std::string Utf8(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return std::string(text.begin(), text.end());
}

[[nodiscard]] std::filesystem::path FromUtf8(const std::string& utf8) {
  return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

}  // namespace

LibraryHost::LibraryHost() = default;

LibraryHost::~LibraryHost() {
  Stop();
}

bool LibraryHost::Start(const std::filesystem::path& database_path) {
  try {
    library_ = std::make_unique<library::Library>(database_path);
  } catch (const std::exception& error) {
    // Not fatal, and not silent: the capability goes off and the message is the
    // reason the page will show.
    const std::lock_guard<std::mutex> lock(mutex_);
    last_error_ = error.what();
    library_.reset();
    return false;
  }

  scanner_ = std::make_unique<library::Scanner>(*library_, library::MakeTagLibReader());

  const std::lock_guard<std::mutex> lock(mutex_);
  if (const std::optional<std::string> remembered = library_->GetSetting(kRootSetting);
      remembered.has_value()) {
    root_ = *remembered;
  }
  return true;
}

void LibraryHost::Stop() {
  if (status_timer_) {
    status_timer_->Cancel();
    status_timer_ = nullptr;
  }
  if (scanner_) {
    // Asked to stop before the join, or this waits for a scan of fifty thousand
    // files to finish while the window is trying to close.
    scanner_->Cancel();
  }
  if (thread_.joinable()) {
    thread_.join();
  }
  scanner_.reset();
  library_.reset();
}

std::string LibraryHost::description() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (library_ == nullptr) {
    return last_error_.empty() ? "not started" : last_error_;
  }
  return std::to_string(library_->TrackCount()) + " track(s), " +
         std::to_string(library_->CoverCount()) + " cover(s)" +
         (root_.empty() ? std::string(", no folder chosen") : ", " + root_);
}

bool LibraryHost::StartScan(const std::filesystem::path& root) {
  if (library_ == nullptr) {
    return false;
  }
  // One at a time. Two scans of the same folder would be two threads writing the
  // same rows for no gain -- and the answer to "scan while scanning" is not an
  // error, it is "already doing that", which is what the caller is told.
  bool expected = false;
  if (!scanning_.compare_exchange_strong(expected, true)) {
    return false;
  }

  std::filesystem::path target = root;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (target.empty()) {
      target = FromUtf8(root_);
    }
    if (target.empty()) {
      // Nothing to scan and nothing wrong: a fresh install has not been pointed
      // at a folder yet. Recording an error here would make the page show one on
      // its first run, which is exactly the wrong first impression -- the empty
      // root in the status is what says there is no folder.
      scanning_.store(false);
      return false;
    }
    // Recorded here, on the calling thread, rather than inside the scan: the
    // startup line printed "scanning" with no folder after it, because it read
    // the status before the new thread had got as far as writing it down.
    root_ = Utf8(target);
    progress_ = library::ScanProgress{};
    last_error_.clear();
  }

  // The previous thread has finished -- scanning_ was false -- but it has not
  // been joined yet.
  if (thread_.joinable()) {
    thread_.join();
  }
  thread_ = std::thread([this, target] { RunScan(target); });
  return true;
}

void LibraryHost::RunScan(std::filesystem::path root) {
  // Remembered before the scan rather than after: a scan that is interrupted
  // still told us which folder the person meant.
  try {
    library_->SetSetting(kRootSetting, Utf8(root));
  } catch (const std::exception& error) {
    const std::lock_guard<std::mutex> lock(mutex_);
    last_error_ = error.what();
  }

  try {
    const library::ScanProgress result =
        scanner_->Scan(root, [this](const library::ScanProgress& update) {
          const std::lock_guard<std::mutex> lock(mutex_);
          progress_ = update;
        });
    // Said out loud on the way out, because "did the scan finish" is the first
    // question anyone asks when the interface stops answering, and the status
    // event only reaches a page that is still listening.
    std::printf(
        "library: scan finished: %d seen, %d read, +%d ~%d -%d, %d cover(s), %d failed%s\n",
        result.files_seen, result.files_read, result.added, result.updated, result.removed,
        result.covers, result.failed, result.cancelled ? " (cancelled)" : "");
    std::fflush(stdout);

    const std::lock_guard<std::mutex> lock(mutex_);
    progress_ = result;
  } catch (const std::exception& error) {
    const std::lock_guard<std::mutex> lock(mutex_);
    last_error_ = error.what();
  }

  // Last, so that a status read after this never says "scanning" about a thread
  // that has already gone.
  scans_.fetch_add(1, std::memory_order_relaxed);
  scanning_.store(false);
}

void LibraryHost::StartStatusEvents(bridge::EventSink& sink) {
  if (library_ == nullptr) {
    return;
  }
  bridge::EventSink* sink_pointer = &sink;
  status_timer_ = ShellTimer::Every(kStatusSampleMs, [this, sink_pointer] {
    if (library_ == nullptr) {
      return;
    }
    bridge::LibraryStatusEvent payload;
    payload.library = Status();

    // Nothing has changed and nothing is running: an event now would be a
    // heartbeat the page has no use for. The scanning case always emits, because
    // the counters move within a single sample.
    // The scan counter is part of the fingerprint, so a scan that changed
    // nothing still produces exactly one event. Without it, pressing Rescan on
    // an unchanged folder was indistinguishable from a button that does not
    // work: the scan ran, found nothing to do, and the status it produced was
    // byte for byte the one the page already had.
    const std::string fingerprint =
        std::to_string(scans_.load()) + payload.library.ToJson().dump();
    if (!payload.library.scanning && fingerprint == last_emitted_) {
      return;
    }
    last_emitted_ = fingerprint;
    bridge::Events(*sink_pointer).LibraryStatus(payload);
  });
}

bridge::LibraryStatus LibraryHost::Status() const {
  bridge::LibraryStatus status;
  status.scanning = scanning_.load();

  if (library_ != nullptr) {
    // Three counts rather than one query returning all three: they are separate
    // aggregates over the same indexed table, and a list view asks for them once
    // per scan tick, not once per row.
    status.trackCount = library_->TrackCount();
    status.albumCount = library_->AlbumCount();
    status.artistCount = library_->ArtistCount();
  }

  const std::lock_guard<std::mutex> lock(mutex_);
  status.root = root_;
  status.filesSeen = progress_.files_seen;
  status.filesRead = progress_.files_read;
  status.added = progress_.added;
  status.updated = progress_.updated;
  status.removed = progress_.removed;
  status.failed = progress_.failed;
  status.lastError = last_error_;
  return status;
}

std::string LibraryHost::ArtUrl(const std::string& cover_hash) {
  if (cover_hash.empty()) {
    return {};
  }
  // Served from the app's own origin rather than a sonora://art host of its own.
  // A second host would be a second origin, which would mean an img-src
  // exception in the page's content security policy -- and the policy being
  // exactly "'self'" is worth more than the tidier URL.
  return std::string(kSonoraAppOrigin) + kSonoraArtPath + cover_hash;
}

std::optional<library::Cover> LibraryHost::Cover(const std::string& cover_hash) const {
  if (library_ == nullptr) {
    return std::nullopt;
  }
  return library_->GetCover(cover_hash);
}

bridge::LibraryTrack LibraryHost::ToBridge(const library::Track& track) {
  bridge::LibraryTrack out;
  out.id = track.id;
  out.title = track.title;
  out.artist = track.artist;
  out.album = track.album;
  out.albumArtist = track.album_artist;
  out.trackNumber = track.track_number;
  out.discNumber = track.disc_number;
  out.year = track.year;
  out.durationMs = track.duration_ms;
  out.artUrl = ArtUrl(track.cover_hash);
  // And no path. That absence is the point of week 7's second half: the page
  // cannot name a file, so there is no longer anything to validate.
  return out;
}

bridge::LibraryAlbum LibraryHost::ToBridge(const library::AlbumSummary& album) {
  bridge::LibraryAlbum out;
  out.album = album.album;
  out.albumArtist = album.album_artist;
  out.year = album.year;
  out.trackCount = album.track_count;
  out.durationMs = album.duration_ms;
  out.artUrl = ArtUrl(album.cover_hash);
  return out;
}

}  // namespace sonora::shell
