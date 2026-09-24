#include <sonora/bridge/event_coalescer.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace sonora::bridge {

EventCoalescer::EventCoalescer(std::int64_t min_interval_ms, Clock clock, ScheduleFn schedule)
    : min_interval_ms_(min_interval_ms),
      clock_(std::move(clock)),
      schedule_(std::move(schedule)) {
  if (min_interval_ms_ < 0) {
    throw std::invalid_argument("EventCoalescer: min_interval_ms must not be negative");
  }
  if (!clock_ || !schedule_) {
    throw std::invalid_argument("EventCoalescer: clock and schedule are required");
  }
  // Start one interval in the past so the first event goes out immediately.
  // The point of the interval is to bound a stream, not to delay the first
  // thing the page ever hears.
  last_flush_ms_ = clock_() - min_interval_ms_;
}

void EventCoalescer::Post(std::string name, nlohmann::json payload, bool coalesce) {
  ++stats_.posted;

  if (coalesce) {
    const auto existing = std::find_if(pending_.begin(), pending_.end(),
                                       [&](const PendingEvent& e) { return e.name == name; });
    if (existing != pending_.end()) {
      existing->payload = std::move(payload);
      ++stats_.coalesced;
      // Already queued, so the flush is already armed.
      return;
    }
  }

  if (pending_.size() >= kMaxPending) {
    pending_.erase(pending_.begin());
    ++stats_.dropped;
  }
  pending_.push_back(PendingEvent{std::move(name), std::move(payload)});
  ArmFlush();
}

std::vector<PendingEvent> EventCoalescer::Flush() {
  flush_scheduled_ = false;

  const std::int64_t now = clock_();
  if (now - last_flush_ms_ < min_interval_ms_) {
    // Woken early. Do not deliver and do not move last_flush_: just ask again
    // for the time that is actually left.
    if (!pending_.empty()) {
      ArmFlush();
    }
    return {};
  }
  if (pending_.empty()) {
    // Nothing to send, and last_flush_ stays where it was so the next event
    // after a quiet spell is not made to wait for an interval that has already
    // passed.
    return {};
  }

  last_flush_ms_ = now;
  stats_.delivered += static_cast<std::int64_t>(pending_.size());
  return std::exchange(pending_, {});
}

void EventCoalescer::ArmFlush() {
  if (flush_scheduled_) {
    return;
  }
  const std::int64_t earliest = last_flush_ms_ + min_interval_ms_;
  const std::int64_t now = clock_();
  flush_scheduled_ = true;
  schedule_(earliest > now ? earliest - now : 0);
}

}  // namespace sonora::bridge
