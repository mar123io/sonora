#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

#include <sonora/audio/format.h>

namespace sonora::audio {

// Thrown when a source cannot be opened. Decoding failures after that are
// reported as a short read, not as an exception: Read() is called from the
// decode thread, which is not real-time, but the engine above it has to treat
// "no more samples" the same whether the file ended or the file broke.
class DecoderError : public std::runtime_error {
 public:
  explicit DecoderError(const std::string& message) : std::runtime_error(message) {}
};

// One audio source, already converted to interleaved float.
//
// Nothing here knows about devices, threads or timing. That is what makes a
// decoder trivial to fake: the engine tests below run against a generated ramp
// and never touch a file or a sound card.
class Decoder {
 public:
  virtual ~Decoder() = default;

  [[nodiscard]] virtual AudioFormat format() const noexcept = 0;

  // 0 when the length is not known in advance, which is the honest answer for
  // a stream and for some MP3s.
  [[nodiscard]] virtual std::uint64_t total_frames() const noexcept = 0;

  // Reads up to `frames` interleaved frames into `output`, which must have room
  // for frames * channels floats. Returns how many frames were produced; 0
  // means end of stream, and a short read that is not 0 does not.
  virtual std::size_t Read(float* output, std::size_t frames) = 0;

  // False when the source cannot seek, or when the position is past the end.
  virtual bool Seek(std::uint64_t frame) = 0;

 protected:
  Decoder() = default;
};

using DecoderPtr = std::unique_ptr<Decoder>;

}  // namespace sonora::audio
