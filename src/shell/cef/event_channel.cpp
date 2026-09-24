#include "cef/event_channel.h"

#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include "include/cef_frame.h"
#include "include/wrapper/cef_helpers.h"

namespace sonora::shell {
namespace {

// The page's receiving function, injected by nothing: it is defined in
// ui/src/bridge/invoke.ts. Guarded at the call site because the shell can emit
// before the bundle has run, and a ReferenceError in the console is a bad way
// to learn that.
constexpr char kReceiver[] = "window.__sonoraEvents";

// Shown as the source in DevTools when something in the delivery throws, so a
// stack trace does not just say "anonymous".
constexpr char kScriptUrl[] = "sonora://bridge/events";

[[nodiscard]] std::int64_t NowMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

EventChannel::EventChannel(std::int64_t min_interval_ms)
    : coalescer_(
          min_interval_ms,
          [] { return NowMs(); },
          [this](std::int64_t delay_ms) { ScheduleFlush(delay_ms); }) {}

EventChannel::~EventChannel() {
  // A flush already queued inside CEF holds a reference to the timer, not to
  // this object, and a cancelled timer drops the callback that captured it.
  if (pending_flush_) {
    pending_flush_->Cancel();
  }
}

void EventChannel::Emit(std::string_view name, const nlohmann::json& payload, bool coalesce) {
  CEF_REQUIRE_UI_THREAD();
  if (!browser_) {
    // Before the browser exists and after it has gone there is nothing to send
    // to. Queueing here would mean the page's first frame arrives with a
    // backlog of events describing a past it never saw.
    return;
  }
  coalescer_.Post(std::string(name), payload, coalesce);
}

void EventChannel::Attach(CefRefPtr<CefBrowser> browser) {
  CEF_REQUIRE_UI_THREAD();
  browser_ = std::move(browser);
}

void EventChannel::Detach() {
  CEF_REQUIRE_UI_THREAD();
  browser_ = nullptr;
  if (pending_flush_) {
    pending_flush_->Cancel();
    pending_flush_ = nullptr;
  }
}

void EventChannel::ScheduleFlush(std::int64_t delay_ms) {
  // The coalescer asks for exactly one flush per batch, so replacing the
  // handle here cannot drop a live timer.
  pending_flush_ = ShellTimer::Once(delay_ms, [this] { FlushToPage(); });
}

void EventChannel::FlushToPage() {
  CEF_REQUIRE_UI_THREAD();
  pending_flush_ = nullptr;

  std::vector<bridge::PendingEvent> due = coalescer_.Flush();
  if (due.empty() || !browser_) {
    return;
  }

  CefRefPtr<CefFrame> frame = browser_->GetMainFrame();
  if (!frame) {
    return;
  }

  nlohmann::json batch = nlohmann::json::array();
  for (bridge::PendingEvent& event : due) {
    batch.push_back(
        nlohmann::json{{"name", std::move(event.name)}, {"payload", std::move(event.payload)}});
  }

  // One crossing per flush, not per event. The payload travels as a JSON
  // string literal that the page parses, rather than as JavaScript source: a
  // value from the native side is never something V8 evaluates.
  //
  // nlohmann's string dump escapes quotes, backslashes and control characters.
  // It does not escape U+2028 and U+2029, which were line terminators in
  // JavaScript string literals until ES2019 made JSON a subset of JS. Chromium
  // is long past that, and CEF pins the two together, so the assumption is
  // stated here rather than hidden in an escape routine.
  const std::string script = std::string(kReceiver) + " && " + kReceiver + "(" +
                             nlohmann::json(batch.dump()).dump() + ");";
  frame->ExecuteJavaScript(script, kScriptUrl, 0);
}

}  // namespace sonora::shell
