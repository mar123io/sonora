#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

namespace sonora::audio {

// Runs one step function on a thread of its own, for as long as it is alive.
//
// Week 5 hard-wired it to AudioEngine::DecodeStep(); week 6 has two callers --
// Player::Pump() in the shell and a bare stream in the tests -- so it takes
// the step instead of the object. Returns true when it did work, which is all
// this class needs to know to decide whether to come straight back.
//
// It polls. That looks lazy next to a condition variable the device callback
// notifies when it has drained a block, and it is the deliberate choice:
// notify_one() takes the condition variable's mutex, and the audio callback
// must not take a mutex under any circumstances (ADR 0006). A callback that
// blocks on a lock held by a thread the scheduler has just preempted is a gap
// you can hear, and it happens exactly when the machine is busy.
//
// The cost is a wake-up every few milliseconds on one thread. The ring holds
// half a second; the poll interval only has to be short next to that.
class DecodeThread {
 public:
  // Returns true when the step did something.
  using Step = std::function<bool()>;

  // `idle_sleep` is how long to wait after a step that produced nothing --
  // either the buffers were full or the source has ended.
  DecodeThread(Step step, std::chrono::milliseconds idle_sleep);
  ~DecodeThread();

  DecodeThread(const DecodeThread&) = delete;
  DecodeThread& operator=(const DecodeThread&) = delete;

  // Idempotent, and called by the destructor. Returns once the thread is gone,
  // so the engine it borrows can safely be destroyed afterwards.
  void Stop();

 private:
  void Run();

  Step step_;
  std::chrono::milliseconds idle_sleep_;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

}  // namespace sonora::audio
