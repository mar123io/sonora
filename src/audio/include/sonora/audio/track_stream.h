#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <sonora/audio/decoder.h>
#include <sonora/audio/format.h>
#include <sonora/audio/ring_buffer.h>

namespace sonora::audio {

// One source, decoded ahead of time into a ring buffer.
//
// This was AudioEngine in week 5. It kept the name for as long as it was the
// only thing there was; now that Player sits above it and owns the queue, the
// volume and the state, "engine" described the wrong object. What is left here
// is exactly one track and the buffer in front of it.
//
// Two halves, deliberately separate calls rather than a class that owns a
// thread:
//
//   DecodeStep()  runs wherever the caller likes, may block, may allocate
//   Read()        runs on the device thread and may do neither
//
// Read() obeys docs/adr/0006-the-audio-callback-is-real-time.md. It returns a
// short count rather than filling with silence: whether a short read is an
// underrun or the end of a track is a question only Player can answer, because
// only Player knows whether another track is waiting.
class TrackStream {
 public:
  struct Config {
    std::size_t ring_frames = 24000;  // half a second at 48 kHz
    std::size_t decode_chunk_frames = 4096;
  };

  // The decoder must already be producing `format`: Player opens every track
  // at the device's format so that a track change needs no conversion and no
  // reopened device. See OpenFileDecoder's target parameter.
  TrackStream(DecoderPtr decoder, Config config);
  ~TrackStream();

  TrackStream(const TrackStream&) = delete;
  TrackStream& operator=(const TrackStream&) = delete;

  // Producer side. Decodes at most one chunk; 0 means the ring is full or the
  // source has ended, and exhausted() tells those apart.
  std::size_t DecodeStep();

  // Fills the ring completely. Producer side.
  std::size_t Prime();

  // REAL TIME. Copies up to `frames` into `output` and returns how many it
  // actually wrote. Writes nothing past that: the caller owns the rest.
  [[nodiscard]] std::size_t Read(float* output, std::size_t frames) noexcept;

  // Producer side. Moves the decoder and empties the ring; the caller must
  // have stopped Read() from running first -- Player does that by holding the
  // stream out of the render path while a seek is in flight.
  bool SeekFrame(std::uint64_t frame);

  [[nodiscard]] AudioFormat format() const noexcept { return format_; }
  [[nodiscard]] std::uint64_t total_frames() const noexcept { return total_frames_; }

  // Frames handed to Read() so far. The position a listener would recognise,
  // as opposed to how far the decoder has run ahead.
  [[nodiscard]] std::uint64_t frames_read() const noexcept {
    return frames_read_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] std::uint64_t frames_decoded() const noexcept {
    return frames_decoded_.load(std::memory_order_relaxed);
  }

  // The decoder has no more samples. There may still be audio in the ring.
  [[nodiscard]] bool exhausted() const noexcept {
    return exhausted_.load(std::memory_order_acquire);
  }

  // Exhausted and drained: nothing of this track will ever be heard again.
  [[nodiscard]] bool finished() const noexcept {
    return exhausted() && ring_.ReadableFrames() == 0;
  }

  // What is still buffered, in frames. Player uses it to decide when there is
  // enough time in hand to open the next track.
  [[nodiscard]] std::size_t buffered_frames() const noexcept { return ring_.ReadableFrames(); }

 private:
  DecoderPtr decoder_;
  Config config_;
  AudioFormat format_{};
  std::uint64_t total_frames_ = 0;

  RingBuffer ring_;
  std::vector<float> decode_scratch_;

  std::atomic<std::uint64_t> frames_read_{0};
  std::atomic<std::uint64_t> frames_decoded_{0};
  std::atomic<bool> exhausted_{false};
};

}  // namespace sonora::audio
