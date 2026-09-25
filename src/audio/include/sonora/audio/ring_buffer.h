#pragma once

#include <atomic>
#include <cstddef>
#include <vector>

#include <sonora/audio/format.h>

namespace sonora::audio {

// A lock-free ring buffer for exactly one producer and one consumer.
//
// This is the seam between the decode thread, which may block on a file and
// take as long as it likes, and the device callback, which may not. Everything
// on the consumer side of this class runs under the rule in
// docs/adr/0006-the-audio-callback-is-real-time.md: no allocation, no locks, no
// I/O, no logging.
//
// "Lock-free" here is not a performance claim, it is a correctness one. A mutex
// held by the decode thread while the operating system preempts it is a mutex
// the audio callback would wait on, and a callback that waits produces a gap
// you can hear. Priority inversion does not get better under load; it gets
// worse exactly when the machine is busy, which is when anyone notices.
//
// Single producer, single consumer is the whole contract. Two producers, or two
// consumers, silently corrupt it -- there is no check for that, because a check
// would cost something on the path that must not pay.
class RingBuffer {
 public:
  // Capacity is rounded up to a power of two so the index wrap is a mask
  // rather than a division. The buffer always holds one frame fewer than its
  // capacity: the full and empty states have to be told apart, and giving up
  // one frame is cheaper than carrying a separate flag across threads.
  RingBuffer(std::size_t capacity_frames, int channels);

  RingBuffer(const RingBuffer&) = delete;
  RingBuffer& operator=(const RingBuffer&) = delete;

  // Producer side. Writes as many of `frames` as fit and returns how many were
  // taken; a short write means the consumer is behind, which is normal.
  std::size_t Write(const float* source, std::size_t frames) noexcept;

  // Consumer side, real-time. Reads as many of `frames` as are available and
  // returns how many were given; a short read is an underrun, and it is the
  // caller's job to fill the rest with silence rather than leave stale samples.
  std::size_t Read(float* destination, std::size_t frames) noexcept;

  // Both are approximations the instant they return, because the other side is
  // running. They are exact in the direction that matters: ReadableFrames never
  // over-reports to the consumer, WritableFrames never over-reports to the
  // producer, so acting on a stale value is safe.
  [[nodiscard]] std::size_t ReadableFrames() const noexcept;
  [[nodiscard]] std::size_t WritableFrames() const noexcept;

  // Only when neither side is running.
  void Reset() noexcept;

  [[nodiscard]] std::size_t capacity_frames() const noexcept { return capacity_frames_ - 1; }
  [[nodiscard]] int channels() const noexcept { return channels_; }

 private:
  [[nodiscard]] std::size_t Mask(std::size_t position) const noexcept {
    return position & (capacity_frames_ - 1);
  }

  std::vector<float> samples_;
  std::size_t capacity_frames_;  // a power of two
  int channels_;

  // Monotonically increasing, masked on use. They never wrap in practice: at
  // 48 kHz a 64-bit counter takes about twelve million years to overflow.
  //
  // alignas keeps the two counters off the same cache line. Without it the
  // producer's store invalidates the line the consumer is reading, on every
  // write, for no reason other than layout -- the two threads would fight over
  // one line while touching different variables.
#if defined(_MSC_VER)
  // C4324: "structure was padded due to alignment specifier". That is not a
  // side effect here, it is the entire request: 112 bytes of padding is what
  // buys the separation. Suppressed at the declaration rather than on the
  // target, so it stays off for these two members and stays on everywhere
  // else, where an unexpectedly padded struct is worth knowing about.
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
  alignas(64) std::atomic<std::size_t> write_position_{0};
  alignas(64) std::atomic<std::size_t> read_position_{0};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
};

}  // namespace sonora::audio
