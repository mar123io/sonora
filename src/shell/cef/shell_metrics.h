#pragma once

#include <chrono>
#include <cstdint>

namespace sonora::shell {

// The few numbers diagnostics.getMetrics reports about the process itself.
//
// Deliberately small, and deliberately not atomic: everything here is touched
// from the CEF UI thread only. Memory and CPU figures, which do need the
// operating system, belong in the platform layer and arrive with the
// performance gates in week 12.
class ShellMetrics {
 public:
  ShellMetrics() : started_(std::chrono::steady_clock::now()) {}

  void NoteQuery() { ++queries_handled_; }

  [[nodiscard]] std::int64_t uptime_ms() const {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now() - started_).count();
  }

  [[nodiscard]] std::int64_t queries_handled() const { return queries_handled_; }

 private:
  std::chrono::steady_clock::time_point started_;
  std::int64_t queries_handled_ = 0;
};

}  // namespace sonora::shell
