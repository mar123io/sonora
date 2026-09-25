#pragma once

#include <cstddef>
#include <cstdint>

namespace sonora::audio {

// Everything inside this target speaks one sample type: 32-bit float,
// interleaved, in [-1, 1]. Integer formats are converted once, at the decoder
// boundary, and never appear again.
//
// The alternative -- carrying the source format through the engine -- means
// every stage has to handle every combination, and the conversions end up
// scattered across the code in the places where someone noticed they were
// missing.
struct AudioFormat {
  int sample_rate_hz = 0;
  int channels = 0;

  [[nodiscard]] constexpr bool IsValid() const noexcept {
    return sample_rate_hz > 0 && channels > 0;
  }

  [[nodiscard]] constexpr std::size_t SamplesPerFrame() const noexcept {
    return static_cast<std::size_t>(channels);
  }

  [[nodiscard]] constexpr bool operator==(const AudioFormat&) const noexcept = default;
};

// Frames, not samples, not bytes. A frame is one sample per channel, and it is
// the only unit that means the same thing at every stage; mixing the three is
// how a stereo file ends up playing at half speed.
[[nodiscard]] constexpr std::int64_t FramesToMilliseconds(std::uint64_t frames,
                                                          int sample_rate_hz) noexcept {
  if (sample_rate_hz <= 0) {
    return 0;
  }
  return static_cast<std::int64_t>((frames * 1000ULL) /
                                   static_cast<std::uint64_t>(sample_rate_hz));
}

[[nodiscard]] constexpr std::uint64_t MillisecondsToFrames(std::int64_t milliseconds,
                                                           int sample_rate_hz) noexcept {
  if (sample_rate_hz <= 0 || milliseconds < 0) {
    return 0;
  }
  return (static_cast<std::uint64_t>(milliseconds) *
          static_cast<std::uint64_t>(sample_rate_hz)) /
         1000ULL;
}

}  // namespace sonora::audio
