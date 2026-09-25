#include <sonora/audio/ring_buffer.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace sonora::audio {
namespace {

[[nodiscard]] std::size_t RoundUpToPowerOfTwo(std::size_t value) noexcept {
  std::size_t result = 1;
  while (result < value) {
    result <<= 1;
  }
  return result;
}

}  // namespace

RingBuffer::RingBuffer(std::size_t capacity_frames, int channels)
    : capacity_frames_(RoundUpToPowerOfTwo(capacity_frames + 1)), channels_(channels) {
  if (channels <= 0) {
    throw std::invalid_argument("RingBuffer: channels must be positive");
  }
  if (capacity_frames == 0) {
    throw std::invalid_argument("RingBuffer: capacity must be positive");
  }
  // The single allocation this class ever makes, and it happens here, off the
  // audio thread. Nothing below allocates.
  samples_.resize(capacity_frames_ * static_cast<std::size_t>(channels_));
}

std::size_t RingBuffer::Write(const float* source, std::size_t frames) noexcept {
  const std::size_t write = write_position_.load(std::memory_order_relaxed);
  // acquire: everything the consumer did before publishing this read position
  // is visible to us, so the space we are about to write into is really free.
  const std::size_t read = read_position_.load(std::memory_order_acquire);

  const std::size_t free_frames = capacity_frames_ - 1 - (write - read);
  const std::size_t to_write = std::min(frames, free_frames);
  if (to_write == 0) {
    return 0;
  }

  const std::size_t start = Mask(write);
  const std::size_t first = std::min(to_write, capacity_frames_ - start);
  const std::size_t channels = static_cast<std::size_t>(channels_);

  std::memcpy(samples_.data() + start * channels, source, first * channels * sizeof(float));
  if (to_write > first) {
    std::memcpy(samples_.data(), source + first * channels,
                (to_write - first) * channels * sizeof(float));
  }

  // release: the samples above are visible to the consumer before it can see
  // the new position. Without this it can read the position and then the old
  // contents, which is silence at best and last week's audio at worst.
  write_position_.store(write + to_write, std::memory_order_release);
  return to_write;
}

std::size_t RingBuffer::Read(float* destination, std::size_t frames) noexcept {
  const std::size_t read = read_position_.load(std::memory_order_relaxed);
  const std::size_t write = write_position_.load(std::memory_order_acquire);

  const std::size_t available = write - read;
  const std::size_t to_read = std::min(frames, available);
  if (to_read == 0) {
    return 0;
  }

  const std::size_t start = Mask(read);
  const std::size_t first = std::min(to_read, capacity_frames_ - start);
  const std::size_t channels = static_cast<std::size_t>(channels_);

  std::memcpy(destination, samples_.data() + start * channels,
              first * channels * sizeof(float));
  if (to_read > first) {
    std::memcpy(destination + first * channels, samples_.data(),
                (to_read - first) * channels * sizeof(float));
  }

  read_position_.store(read + to_read, std::memory_order_release);
  return to_read;
}

std::size_t RingBuffer::ReadableFrames() const noexcept {
  const std::size_t write = write_position_.load(std::memory_order_acquire);
  const std::size_t read = read_position_.load(std::memory_order_relaxed);
  return write - read;
}

std::size_t RingBuffer::WritableFrames() const noexcept {
  const std::size_t write = write_position_.load(std::memory_order_relaxed);
  const std::size_t read = read_position_.load(std::memory_order_acquire);
  return capacity_frames_ - 1 - (write - read);
}

void RingBuffer::Reset() noexcept {
  write_position_.store(0, std::memory_order_relaxed);
  read_position_.store(0, std::memory_order_relaxed);
}

}  // namespace sonora::audio
