#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

#include <sonora/bridge/event_coalescer.h>

using namespace sonora::bridge;

namespace {

// Time is a parameter here, not something to wait for. A test that sleeps for
// a quarter of a second to watch a rate limiter is slow, and flaky on a loaded
// CI runner exactly when it matters.
class Harness {
 public:
  explicit Harness(std::int64_t interval_ms = 250)
      : coalescer_(
            interval_ms,
            [this] { return now_ms_; },
            [this](std::int64_t delay) { scheduled_ = now_ms_ + delay; }) {}

  EventCoalescer& coalescer() { return coalescer_; }

  void Advance(std::int64_t ms) { now_ms_ += ms; }

  [[nodiscard]] bool has_scheduled_flush() const { return scheduled_.has_value(); }

  // Runs the flush the coalescer asked for, moving time to when it asked for
  // it. Returns what came out.
  std::vector<PendingEvent> RunScheduledFlush() {
    REQUIRE(scheduled_.has_value());
    now_ms_ = std::max(now_ms_, *scheduled_);
    scheduled_.reset();
    return coalescer_.Flush();
  }

  // Calls Flush without waiting, the way an early wake-up would.
  std::vector<PendingEvent> FlushNow() {
    scheduled_.reset();
    return coalescer_.Flush();
  }

  [[nodiscard]] std::int64_t scheduled_delay() const {
    REQUIRE(scheduled_.has_value());
    return *scheduled_ - now_ms_;
  }

 private:
  std::int64_t now_ms_ = 1'000;
  std::optional<std::int64_t> scheduled_;
  EventCoalescer coalescer_;
};

nlohmann::json Tick(int sequence) {
  return nlohmann::json{{"sequence", sequence}};
}

}  // namespace

TEST_CASE("the first event goes out immediately", "[events]") {
  // The interval bounds a stream. It is not a reason to make the page wait a
  // quarter of a second for the first thing it ever hears.
  Harness harness;
  harness.coalescer().Post("diagnostics.heartbeat", Tick(1), true);

  REQUIRE(harness.scheduled_delay() == 0);
  const auto delivered = harness.RunScheduledFlush();
  REQUIRE(delivered.size() == 1);
  REQUIRE(delivered[0].payload["sequence"] == 1);
}

TEST_CASE("a burst collapses to the newest value", "[events]") {
  Harness harness;
  auto& coalescer = harness.coalescer();

  coalescer.Post("diagnostics.heartbeat", Tick(1), true);
  REQUIRE(harness.RunScheduledFlush().size() == 1);

  // Twenty ticks inside one interval, the way a producer at 20 Hz behaves
  // against a consumer at 4 Hz.
  for (int i = 2; i <= 21; ++i) {
    harness.Advance(10);
    coalescer.Post("diagnostics.heartbeat", Tick(i), true);
  }

  REQUIRE(coalescer.pending() == 1);
  const auto delivered = harness.RunScheduledFlush();
  REQUIRE(delivered.size() == 1);
  REQUIRE(delivered[0].payload["sequence"] == 21);

  const auto& stats = coalescer.stats();
  REQUIRE(stats.posted == 21);
  REQUIRE(stats.delivered == 2);
  REQUIRE(stats.coalesced == 19);
  REQUIRE(stats.dropped == 0);
}

TEST_CASE("different events do not coalesce into each other", "[events]") {
  Harness harness;
  auto& coalescer = harness.coalescer();

  coalescer.Post("a.one", Tick(1), true);
  coalescer.Post("b.two", Tick(2), true);
  coalescer.Post("a.one", Tick(3), true);

  const auto delivered = harness.RunScheduledFlush();
  REQUIRE(delivered.size() == 2);
  REQUIRE(delivered[0].name == "a.one");
  REQUIRE(delivered[0].payload["sequence"] == 3);  // replaced in place
  REQUIRE(delivered[1].name == "b.two");
}

TEST_CASE("a non-coalescing event keeps every occurrence", "[events]") {
  // Progress is a value; "track finished" is a fact. Dropping the second one
  // loses information no later message carries.
  Harness harness;
  auto& coalescer = harness.coalescer();

  coalescer.Post("player.trackEnded", Tick(1), false);
  coalescer.Post("player.trackEnded", Tick(2), false);
  coalescer.Post("player.trackEnded", Tick(3), false);

  const auto delivered = harness.RunScheduledFlush();
  REQUIRE(delivered.size() == 3);
  REQUIRE(delivered[2].payload["sequence"] == 3);
  REQUIRE(coalescer.stats().coalesced == 0);
}

TEST_CASE("an early flush delivers nothing and asks again", "[events]") {
  Harness harness;
  auto& coalescer = harness.coalescer();

  coalescer.Post("diagnostics.heartbeat", Tick(1), true);
  REQUIRE(harness.RunScheduledFlush().size() == 1);

  harness.Advance(50);
  coalescer.Post("diagnostics.heartbeat", Tick(2), true);
  REQUIRE(harness.scheduled_delay() == 200);

  harness.Advance(10);
  REQUIRE(harness.FlushNow().empty());  // woken 190 ms early
  REQUIRE(coalescer.pending() == 1);    // and nothing was lost
  REQUIRE(harness.has_scheduled_flush());
  REQUIRE(harness.scheduled_delay() == 190);

  REQUIRE(harness.RunScheduledFlush().size() == 1);
}

TEST_CASE("a quiet spell does not delay the next event", "[events]") {
  // If last_flush moved on an empty flush, an event arriving after a minute of
  // silence would still wait for an interval that has long since passed.
  Harness harness;
  auto& coalescer = harness.coalescer();

  coalescer.Post("diagnostics.heartbeat", Tick(1), true);
  REQUIRE(harness.RunScheduledFlush().size() == 1);

  harness.Advance(60'000);
  REQUIRE(harness.FlushNow().empty());

  coalescer.Post("diagnostics.heartbeat", Tick(2), true);
  REQUIRE(harness.scheduled_delay() == 0);
  REQUIRE(harness.RunScheduledFlush().size() == 1);
}

TEST_CASE("the queue is bounded when nobody flushes", "[events]") {
  // Reaching this means the host has stopped flushing, which is a bug. An
  // unbounded queue turns that bug into an out-of-memory; a counter is easier
  // to find and cheaper to survive.
  Harness harness;
  auto& coalescer = harness.coalescer();

  const int posted = static_cast<int>(EventCoalescer::kMaxPending) + 10;
  for (int i = 0; i < posted; ++i) {
    coalescer.Post("player.trackEnded", Tick(i), false);
  }

  REQUIRE(coalescer.pending() == EventCoalescer::kMaxPending);
  REQUIRE(coalescer.stats().dropped == 10);

  // The oldest went, not the newest: the recent past is the useful part.
  const auto delivered = harness.RunScheduledFlush();
  REQUIRE(delivered.front().payload["sequence"] == 10);
  REQUIRE(delivered.back().payload["sequence"] == posted - 1);
}

TEST_CASE("a flush is armed once per batch, not once per event", "[events]") {
  Harness harness;
  auto& coalescer = harness.coalescer();
  coalescer.Post("diagnostics.heartbeat", Tick(1), true);
  REQUIRE(harness.RunScheduledFlush().size() == 1);

  harness.Advance(10);
  coalescer.Post("a.one", Tick(1), true);
  const std::int64_t first_delay = harness.scheduled_delay();
  coalescer.Post("b.two", Tick(2), true);
  coalescer.Post("c.three", Tick(3), true);

  // Still the same single scheduled flush, at the same time.
  REQUIRE(harness.scheduled_delay() == first_delay);
  REQUIRE(harness.RunScheduledFlush().size() == 3);
}

TEST_CASE("an interval of zero delivers everything at once", "[events]") {
  Harness harness(0);
  auto& coalescer = harness.coalescer();

  coalescer.Post("a.one", Tick(1), false);
  coalescer.Post("a.one", Tick(2), false);

  REQUIRE(harness.scheduled_delay() == 0);
  REQUIRE(harness.RunScheduledFlush().size() == 2);
}

TEST_CASE("the coalescer refuses to be built without a clock", "[events]") {
  REQUIRE_THROWS(EventCoalescer(250, nullptr, [](std::int64_t) {}));
  REQUIRE_THROWS(EventCoalescer(250, [] { return std::int64_t{0}; }, nullptr));
  REQUIRE_THROWS(EventCoalescer(-1, [] { return std::int64_t{0}; }, [](std::int64_t) {}));
}
