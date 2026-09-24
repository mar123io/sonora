#include "cef/timer.h"

#include <utility>

#include "include/wrapper/cef_helpers.h"

namespace sonora::shell {

ShellTimer::ShellTimer(std::int64_t interval_ms, bool repeating, Callback callback)
    : interval_ms_(interval_ms), repeating_(repeating), callback_(std::move(callback)) {}

CefRefPtr<ShellTimer> ShellTimer::Once(std::int64_t delay_ms, Callback callback) {
  CefRefPtr<ShellTimer> timer =
      new ShellTimer(delay_ms, /*repeating=*/false, std::move(callback));
  CefPostDelayedTask(TID_UI, timer, delay_ms);
  return timer;
}

CefRefPtr<ShellTimer> ShellTimer::Every(std::int64_t interval_ms, Callback callback) {
  CefRefPtr<ShellTimer> timer =
      new ShellTimer(interval_ms, /*repeating=*/true, std::move(callback));
  CefPostDelayedTask(TID_UI, timer, interval_ms);
  return timer;
}

void ShellTimer::Cancel() {
  cancelled_ = true;
  // The callback is released here and not at the next run: it is the thing
  // most likely to be holding a pointer to whatever is being torn down.
  callback_ = nullptr;
}

void ShellTimer::Execute() {
  CEF_REQUIRE_UI_THREAD();
  if (cancelled_ || !callback_) {
    return;
  }

  callback_();

  // Re-checked: the callback is allowed to cancel its own timer, and a repeat
  // posted after that would be one run too many.
  if (repeating_ && !cancelled_) {
    CefPostDelayedTask(TID_UI, this, interval_ms_);
  }
}

}  // namespace sonora::shell
