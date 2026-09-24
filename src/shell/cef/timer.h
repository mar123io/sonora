#pragma once

#include <cstdint>
#include <functional>

#include "include/cef_task.h"

namespace sonora::shell {

// A cancellable one-shot or repeating callback on the CEF UI thread.
//
// CEF posts tasks but does not cancel them: once CefPostDelayedTask has taken a
// task it will run, and a task holding a raw pointer to something that has been
// destroyed in the meantime is a use-after-free waiting for a slow machine. The
// task being reference counted is what makes the object outlive its owner
// safely; Cancel() is what makes it harmless when it does.
//
// Every method belongs to the UI thread. The flag is a plain bool for that
// reason and not an atomic pretending the class is thread-safe.
class ShellTimer final : public CefTask {
 public:
  using Callback = std::function<void()>;

  // Runs `callback` once, `delay_ms` from now.
  [[nodiscard]] static CefRefPtr<ShellTimer> Once(std::int64_t delay_ms, Callback callback);

  // Runs `callback` every `interval_ms`, starting one interval from now, until
  // Cancel(). The interval is measured from the end of one run to the start of
  // the next, so a slow callback slows the timer down instead of queueing.
  [[nodiscard]] static CefRefPtr<ShellTimer> Every(std::int64_t interval_ms, Callback callback);

  // After this the callback never runs again, including from a task already
  // queued inside CEF. Calling it twice, or after the last run, is fine.
  void Cancel();

  [[nodiscard]] bool cancelled() const { return cancelled_; }

  // CefTask
  void Execute() override;

  ShellTimer(const ShellTimer&) = delete;
  ShellTimer& operator=(const ShellTimer&) = delete;

 private:
  ShellTimer(std::int64_t interval_ms, bool repeating, Callback callback);
  ~ShellTimer() override = default;

  std::int64_t interval_ms_;
  bool repeating_;
  bool cancelled_ = false;
  Callback callback_;

  IMPLEMENT_REFCOUNTING(ShellTimer);
};

}  // namespace sonora::shell
