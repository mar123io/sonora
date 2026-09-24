#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

// Rate-limits the native side's events on their way to the page.
//
// The producer and the consumer of an event run at different speeds and always
// will: an audio callback fires every few milliseconds, a download reports
// every chunk, and a human reads a number four times a second at most. Sending
// one message per production is how a UI thread ends up spending its time
// parsing JSON about a progress bar.
//
// The rule here is "latest wins, at most once per interval". An event marked
// coalescing replaces the pending one with the same name instead of queueing
// behind it, so the page always gets the newest value and never a backlog of
// stale ones. Events not marked coalescing queue normally, because for those
// each occurrence is the message.
//
// Threading: one thread. The shell posts from the CEF UI thread and CEF's own
// task posting is what gets other threads onto it. That is a deliberate choice
// over an internal mutex -- week 5's audio callback must not block on a lock
// held by whoever is serialising JSON.

namespace sonora::bridge {

struct PendingEvent {
  std::string name;
  nlohmann::json payload;
};

class EventCoalescer {
 public:
  // Milliseconds from a monotonic clock. Injected so the tests can move time
  // themselves; a test that sleeps is a test that is slow and flaky at once.
  using Clock = std::function<std::int64_t()>;

  // Asks the host to call Flush() in `delay_ms`. Called at most once per
  // pending batch.
  using ScheduleFn = std::function<void(std::int64_t delay_ms)>;

  // Above this many pending events the oldest are dropped. Reached only if the
  // host stops calling Flush entirely, which is a bug -- but an unbounded queue
  // turns that bug into an out-of-memory instead of a counter going up.
  static constexpr std::size_t kMaxPending = 256;

  struct Stats {
    std::int64_t posted = 0;     // handed to Post()
    std::int64_t delivered = 0;  // handed back by Flush()
    std::int64_t coalesced = 0;  // replaced by a newer event of the same name
    std::int64_t dropped = 0;    // discarded because the queue was full
  };

  EventCoalescer(std::int64_t min_interval_ms, Clock clock, ScheduleFn schedule);

  void Post(std::string name, nlohmann::json payload, bool coalesce);

  // Everything due now, in the order it was first posted. Empty when called
  // before the interval has elapsed -- in which case another flush is
  // scheduled, so an early wake-up costs nothing but does not lose events.
  [[nodiscard]] std::vector<PendingEvent> Flush();

  [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
  [[nodiscard]] std::size_t pending() const noexcept { return pending_.size(); }
  [[nodiscard]] std::int64_t min_interval_ms() const noexcept { return min_interval_ms_; }

 private:
  void ArmFlush();

  std::int64_t min_interval_ms_;
  Clock clock_;
  ScheduleFn schedule_;
  std::vector<PendingEvent> pending_;
  bool flush_scheduled_ = false;
  std::int64_t last_flush_ms_ = 0;
  Stats stats_;
};

}  // namespace sonora::bridge
