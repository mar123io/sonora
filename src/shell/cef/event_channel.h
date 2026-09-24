#pragma once

#include <cstdint>
#include <string_view>

#include <bridge_generated.h>
#include <sonora/bridge/event_coalescer.h>

#include "cef/timer.h"
#include "include/cef_browser.h"

namespace sonora::shell {

// Carries events from the native side to the page.
//
// The rate limiting is in src/bridge, where it can be tested without a browser.
// What is left here is the part that needs CEF: a UI-thread timer to drive the
// flush, and one call into the page per batch.
//
// Direction matters. A query is the page asking a question and is answered
// through the message router's callback; an event is the shell saying something
// unprompted, and there is no callback to answer on. The two do not share a
// mechanism, which is why this class exists rather than a persistent query --
// those are refused in bridge_router.cpp for the same reason.
class EventChannel final : public bridge::EventSink {
 public:
  // 4 Hz. Fast enough that a progress number does not look stuck, slow enough
  // that the renderer is not parsing JSON instead of painting.
  static constexpr std::int64_t kDefaultIntervalMs = 250;

  explicit EventChannel(std::int64_t min_interval_ms = kDefaultIntervalMs);
  ~EventChannel() override;

  EventChannel(const EventChannel&) = delete;
  EventChannel& operator=(const EventChannel&) = delete;

  // bridge::EventSink. Called on the UI thread, through the generated Events
  // class rather than directly.
  void Emit(std::string_view name, const nlohmann::json& payload, bool coalesce) override;

  // From SonoraClient's life-span handlers. Between these two calls there is
  // somewhere to send to; outside them an event is dropped where it is made,
  // which is cheaper and more honest than queueing for a page that does not
  // exist.
  void Attach(CefRefPtr<CefBrowser> browser);
  void Detach();

  [[nodiscard]] const bridge::EventCoalescer::Stats& stats() const {
    return coalescer_.stats();
  }

 private:
  void ScheduleFlush(std::int64_t delay_ms);
  void FlushToPage();

  bridge::EventCoalescer coalescer_;
  CefRefPtr<CefBrowser> browser_;
  CefRefPtr<ShellTimer> pending_flush_;
};

}  // namespace sonora::shell
