#pragma once

#include <atomic>
#include <chrono>
#include <thread>

namespace sonora::audio {

class AudioEngine;

// Turns AudioEngine::DecodeStep() into a thread that keeps the ring buffer fed.
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
  // `idle_sleep` is how long to wait after a step that produced nothing --
  // either the ring was full or the source has ended.
  DecodeThread(AudioEngine& engine, std::chrono::milliseconds idle_sleep);
  ~DecodeThread();

  DecodeThread(const DecodeThread&) = delete;
  DecodeThread& operator=(const DecodeThread&) = delete;

  // Idempotent, and called by the destructor. Returns once the thread is gone,
  // so the engine it borrows can safely be destroyed afterwards.
  void Stop();

 private:
  void Run();

  AudioEngine& engine_;
  std::chrono::milliseconds idle_sleep_;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

}  // namespace sonora::audio
