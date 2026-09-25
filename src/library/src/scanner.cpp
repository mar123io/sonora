#include <sonora/library/scanner.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace sonora::library {
namespace {

constexpr int kMaxDefaultWorkers = 4;

// How often progress is reported while the workers are busy.
constexpr auto kProgressInterval = std::chrono::milliseconds(50);

[[nodiscard]] std::string ToUtf8(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return std::string(text.begin(), text.end());
}

[[nodiscard]] std::string LowerAscii(std::string text) {
  // ASCII only, on purpose. This is used on file extensions, which are ASCII in
  // every format this project can decode, and a locale-aware lowercase would
  // make the answer depend on the machine's settings.
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char character) {
    return static_cast<char>(character >= 'A' && character <= 'Z' ? character + ('a' - 'A')
                                                                  : character);
  });
  return text;
}

// The filesystem's own clock, in nanoseconds.
//
// std::filesystem::file_time_type has an unspecified epoch: on Windows it counts
// from 1601 in units of 100 ns, on Linux from 1970 in nanoseconds. None of that
// matters here, because this number is only ever compared against a number this
// same code wrote earlier. It is not a date, and nothing displays it.
[[nodiscard]] std::int64_t ModificationTimeNs(const std::filesystem::directory_entry& entry,
                                              std::error_code& error) {
  const std::filesystem::file_time_type time = entry.last_write_time(error);
  if (error) {
    return 0;
  }
  return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
}

// One file the walk decided is worth reading.
struct Job {
  std::filesystem::path path;
  std::string utf8;
  std::int64_t mtime_ns = 0;
  std::int64_t size_bytes = 0;
  bool existed = false;  // an update rather than an addition
};

}  // namespace

Scanner::Scanner(Library& library, TagReaderPtr reader, ScanOptions options)
    : library_(library), reader_(std::move(reader)), options_(std::move(options)) {
  if (reader_ == nullptr) {
    throw LibraryError("a scanner needs a tag reader");
  }
}

Scanner::~Scanner() = default;

void Scanner::Cancel() noexcept {
  cancelled_.store(true, std::memory_order_relaxed);
}

ScanProgress Scanner::Scan(const std::filesystem::path& root,
                           const std::function<void(const ScanProgress&)>& on_progress) {
  cancelled_.store(false, std::memory_order_relaxed);

  ScanProgress progress;
  const auto report = [&on_progress, &progress] {
    if (on_progress) {
      on_progress(progress);
    }
  };
  const auto finish = [&report, &progress]() -> ScanProgress {
    progress.done = true;
    report();
    return progress;
  };

  std::error_code error;
  const std::filesystem::path absolute_root =
      std::filesystem::absolute(root, error).lexically_normal();
  if (error || !std::filesystem::is_directory(absolute_root, error)) {
    throw LibraryError("not a folder: " + ToUtf8(root));
  }

  // One query for the whole index instead of one per file. A personal library is
  // tens of thousands of rows, so this map is a few megabytes and the scan then
  // touches the database only to write.
  const std::unordered_map<std::string, FileStamp> stamps = library_.Stamps();

  std::vector<Job> jobs;
  std::unordered_set<std::string> seen;

  // ---- the walk, on this thread ------------------------------------------
  //
  // Directory symlinks are not followed: std::filesystem does not follow them
  // unless asked, and asking is how a scan walks into a loop and never comes
  // back. Permission errors skip the entry rather than ending the scan, because
  // a music folder on Windows usually contains at least one thing the current
  // user cannot open.
  std::filesystem::recursive_directory_iterator iterator(
      absolute_root, std::filesystem::directory_options::skip_permission_denied, error);
  const std::filesystem::recursive_directory_iterator end;

  while (!error && iterator != end) {
    if (cancelled_.load(std::memory_order_relaxed)) {
      progress.cancelled = true;
      return finish();
    }

    const std::filesystem::directory_entry entry = *iterator;
    iterator.increment(error);
    if (error) {
      // One unreadable directory does not end the walk.
      error.clear();
      continue;
    }

    std::error_code entry_error;
    if (!entry.is_regular_file(entry_error) || entry_error) {
      continue;
    }
    const std::string extension = LowerAscii(ToUtf8(entry.path().extension()));
    if (std::find(options_.extensions.begin(), options_.extensions.end(), extension) ==
        options_.extensions.end()) {
      continue;
    }

    Job job;
    job.mtime_ns = ModificationTimeNs(entry, entry_error);
    if (entry_error) {
      ++progress.failed;
      continue;
    }
    job.size_bytes = static_cast<std::int64_t>(entry.file_size(entry_error));
    if (entry_error) {
      ++progress.failed;
      continue;
    }

    job.path = entry.path();
    job.utf8 = ToUtf8(entry.path());
    ++progress.files_seen;
    seen.insert(job.utf8);

    const auto known = stamps.find(job.utf8);
    if (known != stamps.end()) {
      if (known->second.mtime_ns == job.mtime_ns &&
          known->second.size_bytes == job.size_bytes) {
        continue;  // The whole point: no tag read, no write, no work.
      }
      job.existed = true;
    }
    jobs.push_back(std::move(job));
  }

  // ---- the tag reads, on the pool ----------------------------------------
  //
  // Workers take jobs by an atomic index and buffer their results locally,
  // flushing a few at a time. The shared state is one index and one small
  // vector, so two threads meet only when a buffer is handed over -- and the
  // expensive part, reading the file, happens with nothing held.
  int worker_count = options_.worker_count;
  if (worker_count <= 0) {
    const unsigned int available = std::thread::hardware_concurrency();
    worker_count = std::clamp(static_cast<int>(available) / 2, 1, kMaxDefaultWorkers);
  }
  worker_count = std::min(worker_count, static_cast<int>(jobs.size()));

  std::atomic<std::size_t> next_job{0};
  std::atomic<int> failures{0};
  std::atomic<int> reads{0};
  std::atomic<int> covers_stored{0};
  std::atomic<int> workers_running{worker_count};

  std::mutex results_mutex;
  std::condition_variable results_ready;
  std::vector<std::pair<Track, bool>> results;  // the track, and whether it existed

  const auto worker = [&] {
    std::vector<std::pair<Track, bool>> local;
    const std::size_t flush_at = 16;

    for (;;) {
      if (cancelled_.load(std::memory_order_relaxed)) {
        break;
      }
      const std::size_t index = next_job.fetch_add(1, std::memory_order_relaxed);
      if (index >= jobs.size()) {
        break;
      }

      const Job& job = jobs[index];
      std::optional<TagData> tags = reader_->Read(job.path, options_.read_covers);
      reads.fetch_add(1, std::memory_order_relaxed);
      if (!tags.has_value()) {
        // Not indexed and not fatal: a broken or half-copied file is a fact
        // about the folder, not an error in the scan.
        failures.fetch_add(1, std::memory_order_relaxed);
        continue;
      }

      Track track;
      track.path = job.utf8;
      track.title = tags->title;
      track.artist = tags->artist;
      track.album = tags->album;
      track.album_artist = tags->album_artist.empty() ? tags->artist : tags->album_artist;
      track.track_number = tags->track_number;
      track.disc_number = tags->disc_number;
      track.year = tags->year;
      track.duration_ms = tags->duration_ms;
      track.mtime_ns = job.mtime_ns;
      track.size_bytes = job.size_bytes;

      // A file with no title tag is not nameless: the file name is what the
      // person called it, and it is the only thing they can search for.
      if (track.title.empty()) {
        track.title = ToUtf8(job.path.stem());
      }

      // The artwork is stored here, on this worker, and the bytes are dropped
      // before the track joins the queue. It matters: an album's cover is a few
      // hundred kilobytes, a batch is 256 tracks, and carrying the pictures
      // through to the writer would mean holding a hundred megabytes of JPEG to
      // write twelve copies of one row.
      //
      // Writing from several threads is exactly what Library's mutex is for, and
      // the insert is OR IGNORE, so two workers finding the same cover at the
      // same moment is not a case to handle.
      if (tags->cover.has_value()) {
        const std::string hash = CoverHash(tags->cover->bytes);
        if (!library_.HasCover(hash)) {
          library_.PutCover(hash, *tags->cover);
          covers_stored.fetch_add(1, std::memory_order_relaxed);
        }
        track.cover_hash = hash;
        tags->cover.reset();
      }

      local.emplace_back(std::move(track), job.existed);
      if (local.size() >= flush_at) {
        const std::lock_guard<std::mutex> lock(results_mutex);
        for (std::pair<Track, bool>& result : local) {
          results.push_back(std::move(result));
        }
        local.clear();
        results_ready.notify_one();
      }
    }

    {
      const std::lock_guard<std::mutex> lock(results_mutex);
      for (std::pair<Track, bool>& result : local) {
        results.push_back(std::move(result));
      }
    }
    workers_running.fetch_sub(1, std::memory_order_release);
    results_ready.notify_one();
  };

  std::vector<std::thread> pool;
  pool.reserve(static_cast<std::size_t>(worker_count));
  for (int i = 0; i < worker_count; ++i) {
    pool.emplace_back(worker);
  }

  // ---- the writes, on this thread ----------------------------------------
  //
  // One thread writes, always this one. Not because SQLite could not take
  // several -- Library serialises them -- but because batching is what makes a
  // scan fast, and a batch is only a batch if one thread is assembling it.
  std::vector<Track> batch;
  int batch_updates = 0;
  const auto flush = [&] {
    if (batch.empty()) {
      return;
    }
    library_.UpsertBatch(batch);
    progress.added += static_cast<int>(batch.size()) - batch_updates;
    progress.updated += batch_updates;
    batch.clear();
    batch_updates = 0;
  };

  for (;;) {
    std::vector<std::pair<Track, bool>> taken;
    {
      std::unique_lock<std::mutex> lock(results_mutex);
      results_ready.wait_for(lock, kProgressInterval, [&] {
        return !results.empty() || workers_running.load(std::memory_order_acquire) == 0;
      });
      taken.swap(results);
    }

    for (std::pair<Track, bool>& result : taken) {
      batch.push_back(std::move(result.first));
      batch_updates += result.second ? 1 : 0;
      if (batch.size() >= options_.batch_size) {
        flush();
      }
    }

    progress.files_read = reads.load(std::memory_order_relaxed);
    progress.covers = covers_stored.load(std::memory_order_relaxed);
    progress.failed += failures.exchange(0, std::memory_order_relaxed);
    report();

    if (workers_running.load(std::memory_order_acquire) == 0 && taken.empty()) {
      const std::lock_guard<std::mutex> lock(results_mutex);
      if (results.empty()) {
        break;
      }
    }
  }

  for (std::thread& thread : pool) {
    thread.join();
  }
  flush();
  progress.files_read = reads.load(std::memory_order_relaxed);
  progress.covers = covers_stored.load(std::memory_order_relaxed);
  progress.failed += failures.exchange(0, std::memory_order_relaxed);

  if (cancelled_.load(std::memory_order_relaxed)) {
    // No removals after a cancelled scan. The walk did not finish, so "not seen"
    // does not mean "not there" -- acting on it would delete half the library.
    progress.cancelled = true;
    return finish();
  }

  // ---- removals ----------------------------------------------------------
  if (options_.remove_missing) {
    // Only what lives under this root. The prefix ends with a separator so that
    // scanning C:\Music never touches rows from C:\Music Backup.
    std::string prefix = ToUtf8(absolute_root);
    const char separator = static_cast<char>(std::filesystem::path::preferred_separator);
    if (!prefix.empty() && prefix.back() != separator && prefix.back() != '/') {
      prefix += separator;
    }

    std::vector<std::int64_t> gone;
    for (const auto& [path, stamp] : stamps) {
      if (path.compare(0, prefix.size(), prefix) != 0) {
        continue;
      }
      if (seen.find(path) == seen.end()) {
        gone.push_back(stamp.id);
      }
    }
    library_.RemoveBatch(gone);
    progress.removed = static_cast<int>(gone.size());

    // An album that was deleted leaves its artwork behind, and artwork is by far
    // the largest thing in the file. Swept here rather than per removal: a
    // cover is shared, so whether it is still needed is only knowable once every
    // row that might refer to it is gone.
    progress.pruned = static_cast<int>(library_.PruneCovers());
  }

  return finish();
}

}  // namespace sonora::library
