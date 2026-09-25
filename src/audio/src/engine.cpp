#include <sonora/audio/engine.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace sonora::audio {
namespace {

[[nodiscard]] float Clamp01(float value) noexcept {
  return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

}  // namespace

AudioEngine::AudioEngine(DecoderPtr decoder, Config config)
    : decoder_(std::move(decoder)),
      config_(config),
      format_(decoder_ ? decoder_->format() : AudioFormat{}),
      total_frames_(decoder_ ? decoder_->total_frames() : 0),
      ring_(config.ring_frames, format_.IsValid() ? format_.channels : 1) {
  if (!decoder_) {
    throw std::invalid_argument("AudioEngine: a decoder is required");
  }
  if (!format_.IsValid()) {
    throw std::invalid_argument("AudioEngine: the decoder reported no usable format");
  }
  if (config_.decode_chunk_frames == 0) {
    throw std::invalid_argument("AudioEngine: decode_chunk_frames must be positive");
  }

  // The scratch buffer is allocated here and never again. Render() and
  // DecodeStep() both run in steady state without touching the allocator --
  // Render because it must not, DecodeStep because there is no reason to.
  decode_scratch_.resize(config_.decode_chunk_frames * format_.SamplesPerFrame());

  const double ramp_frames =
      static_cast<double>(config_.volume_ramp_ms) * format_.sample_rate_hz / 1000.0;
  gain_step_per_frame_ = ramp_frames > 1.0 ? static_cast<float>(1.0 / ramp_frames) : 1.0f;
}

AudioEngine::~AudioEngine() = default;

std::size_t AudioEngine::DecodeStep() {
  if (source_exhausted_.load(std::memory_order_relaxed)) {
    return 0;
  }

  const std::size_t writable = ring_.WritableFrames();
  if (writable == 0) {
    return 0;
  }

  const std::size_t wanted = std::min(writable, config_.decode_chunk_frames);
  const std::size_t decoded = decoder_->Read(decode_scratch_.data(), wanted);
  if (decoded == 0) {
    // release pairs with the acquire in Render: by the time the callback can
    // see "exhausted", the frames written just before it are visible too. The
    // other way round, a short read at the end would be blamed on the decode
    // thread being late rather than on the file being over.
    source_exhausted_.store(true, std::memory_order_release);
    return 0;
  }

  const std::size_t written = ring_.Write(decode_scratch_.data(), decoded);
  frames_decoded_.fetch_add(written, std::memory_order_relaxed);

  // written < decoded cannot happen: `wanted` was clamped to the free space
  // and this is the only producer. Asserted by construction rather than by a
  // runtime check, because a short write here would silently drop audio.
  return written;
}

std::size_t AudioEngine::Prime() {
  // Fills the ring completely, not merely to the last whole chunk: the point
  // is that the first callback cannot underrun, and "nearly full" leaves that
  // to chance. DecodeStep clamps its read to the free space, so the last pass
  // decodes a partial chunk and the one after it returns 0.
  while (ring_.WritableFrames() > 0) {
    if (DecodeStep() == 0) {
      break;
    }
  }
  return ring_.ReadableFrames();
}

void AudioEngine::Render(float* output, std::size_t frames) noexcept {
  const std::size_t channels = format_.SamplesPerFrame();
  const std::size_t got = ring_.Read(output, frames);

  if (got < frames) {
    // Silence, not stale samples: the tail of the buffer still holds whatever
    // the previous callback left there, and playing it back is a stutter you
    // can hear rather than a gap you can hear.
    std::memset(output + got * channels, 0, (frames - got) * channels * sizeof(float));

    // A short read after the source has ended is the end of the track, not a
    // fault. Counting it would mean every successful playback reports one
    // underrun, and a metric that is never zero is a metric nobody reads.
    if (!source_exhausted_.load(std::memory_order_acquire)) {
      underruns_.fetch_add(1, std::memory_order_relaxed);
      frames_missing_.fetch_add(frames - got, std::memory_order_relaxed);
    }
  }

  ApplyGain(output, frames);

  frames_rendered_.fetch_add(frames, std::memory_order_relaxed);
  if (source_exhausted_.load(std::memory_order_acquire) && ring_.ReadableFrames() == 0) {
    finished_.store(true, std::memory_order_relaxed);
  }
}

void AudioEngine::ApplyGain(float* output, std::size_t frames) noexcept {
  const std::size_t channels = format_.SamplesPerFrame();
  const float target = target_gain_.load(std::memory_order_relaxed);

  if (current_gain_ == target) {
    if (current_gain_ == 1.0f) {
      return;  // the common case costs nothing
    }
    for (std::size_t i = 0; i < frames * channels; ++i) {
      output[i] *= current_gain_;
    }
    return;
  }

  // Walk towards the target one frame at a time. A branch per frame is cheap
  // next to what the decoder just did, and it keeps the ramp exact rather than
  // approximately right at the block boundaries.
  float gain = current_gain_;
  for (std::size_t frame = 0; frame < frames; ++frame) {
    if (gain < target) {
      gain = std::min(target, gain + gain_step_per_frame_);
    } else if (gain > target) {
      gain = std::max(target, gain - gain_step_per_frame_);
    }
    for (std::size_t channel = 0; channel < channels; ++channel) {
      output[frame * channels + channel] *= gain;
    }
  }
  current_gain_ = gain;
}

void AudioEngine::SetVolume(float linear) noexcept {
  target_gain_.store(Clamp01(linear), std::memory_order_relaxed);
}

float AudioEngine::volume() const noexcept {
  return target_gain_.load(std::memory_order_relaxed);
}

AudioEngine::Stats AudioEngine::stats() const noexcept {
  Stats stats;
  stats.frames_rendered = frames_rendered_.load(std::memory_order_relaxed);
  stats.frames_decoded = frames_decoded_.load(std::memory_order_relaxed);
  stats.underruns = underruns_.load(std::memory_order_relaxed);
  stats.frames_missing = frames_missing_.load(std::memory_order_relaxed);
  stats.source_exhausted = source_exhausted_.load(std::memory_order_relaxed);
  stats.finished = finished_.load(std::memory_order_relaxed);
  return stats;
}

}  // namespace sonora::audio
