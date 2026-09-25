#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <sonora/audio/decoder.h>
#include <sonora/audio/format.h>
#include <sonora/audio/ring_buffer.h>

namespace sonora::audio {

// One source, decoded ahead of time into a ring buffer and handed to the
// device callback a block at a time.
//
// The two halves are deliberately separate calls rather than one class that
// owns a thread:
//
//   DecodeStep()  runs wherever the caller likes, may block, may allocate
//   Render()      runs on the device thread and may do neither
//
// Splitting them is what makes this testable. A test drives both by hand, in
// whatever order it wants, and can produce an underrun on purpose by calling
// Render() without having called DecodeStep() -- no sleeping, no sound card,
// no flakiness on a loaded CI runner. DecodeThread in decode_thread.h is the
// thin piece that turns DecodeStep() into a running thread in production.
//
// Render() obeys docs/adr/0006-the-audio-callback-is-real-time.md. Read that
// before changing it.
class AudioEngine {
 public:
  struct Config {
    // How much decoded audio to keep ahead of the device. Bigger survives a
    // longer hiccup in the decode thread and costs latency on a seek; this is
    // the only real knob in the engine.
    std::size_t ring_frames = 48000 / 2;  // half a second at 48 kHz
    std::size_t decode_chunk_frames = 4096;
    // A volume change is ramped rather than applied at once, because a step in
    // the sample values is a click, and a click is the single most audible
    // artefact a player can produce.
    int volume_ramp_ms = 20;
  };

  struct Stats {
    std::uint64_t frames_rendered = 0;
    std::uint64_t frames_decoded = 0;
    // A callback that could not be filled while the source still had audio.
    // Counted per callback, not per frame, because one long gap and a hundred
    // short ones are different problems.
    std::uint64_t underruns = 0;
    std::uint64_t frames_missing = 0;
    bool source_exhausted = false;
    bool finished = false;
  };

  AudioEngine(DecoderPtr decoder, Config config);
  ~AudioEngine();

  AudioEngine(const AudioEngine&) = delete;
  AudioEngine& operator=(const AudioEngine&) = delete;

  // Producer side. Decodes at most one chunk and returns how many frames it
  // wrote. 0 means either the ring is full or the source has ended; ask
  // source_exhausted() to tell those apart.
  std::size_t DecodeStep();

  // Fills the ring before the device starts, so the first callback is not an
  // underrun by construction. Returns the frames buffered.
  std::size_t Prime();

  // REAL TIME. Always writes exactly `frames` frames: what the ring has, then
  // silence. Never blocks, never allocates, never fails.
  void Render(float* output, std::size_t frames) noexcept;

  // Safe from any thread. Takes effect over the ramp, inside Render.
  void SetVolume(float linear) noexcept;
  [[nodiscard]] float volume() const noexcept;

  [[nodiscard]] AudioFormat format() const noexcept { return format_; }
  [[nodiscard]] std::uint64_t total_frames() const noexcept { return total_frames_; }
  [[nodiscard]] Stats stats() const noexcept;

  // The source ran out. There may still be audio in the ring.
  [[nodiscard]] bool source_exhausted() const noexcept {
    return source_exhausted_.load(std::memory_order_relaxed);
  }

  // The source ran out and the ring is empty: nothing more will ever be heard.
  [[nodiscard]] bool finished() const noexcept {
    return finished_.load(std::memory_order_relaxed);
  }

 private:
  void ApplyGain(float* output, std::size_t frames) noexcept;

  DecoderPtr decoder_;
  Config config_;
  AudioFormat format_{};
  std::uint64_t total_frames_ = 0;

  RingBuffer ring_;
  std::vector<float> decode_scratch_;  // sized once, in the constructor

  std::atomic<float> target_gain_{1.0f};
  float current_gain_ = 1.0f;  // touched only by Render
  float gain_step_per_frame_ = 1.0f;

  std::atomic<std::uint64_t> frames_rendered_{0};
  std::atomic<std::uint64_t> frames_decoded_{0};
  std::atomic<std::uint64_t> underruns_{0};
  std::atomic<std::uint64_t> frames_missing_{0};
  std::atomic<bool> source_exhausted_{false};
  std::atomic<bool> finished_{false};
};

}  // namespace sonora::audio
