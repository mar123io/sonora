#include <sonora/audio/decode_thread.h>

#include <sonora/audio/engine.h>

namespace sonora::audio {

DecodeThread::DecodeThread(AudioEngine& engine, std::chrono::milliseconds idle_sleep)
    : engine_(engine), idle_sleep_(idle_sleep) {
  thread_ = std::thread([this] { Run(); });
}

DecodeThread::~DecodeThread() {
  Stop();
}

void DecodeThread::Stop() {
  stop_.store(true, std::memory_order_relaxed);
  if (thread_.joinable()) {
    // Joined rather than detached. A detached decode thread outlives the
    // engine it holds a reference to, and the resulting crash happens during
    // shutdown, which is the hardest place to reproduce anything.
    thread_.join();
  }
}

void DecodeThread::Run() {
  while (!stop_.load(std::memory_order_relaxed)) {
    if (engine_.DecodeStep() == 0) {
      // Nothing to do: the ring is full, or the source has ended and this
      // thread's work is over. Either way, stop spinning.
      std::this_thread::sleep_for(idle_sleep_);
    }
  }
}

}  // namespace sonora::audio
