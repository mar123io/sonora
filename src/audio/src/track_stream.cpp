#include <sonora/audio/track_stream.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace sonora::audio {

TrackStream::TrackStream(DecoderPtr decoder, Config config)
    : decoder_(std::move(decoder)),
      config_(config),
      format_(decoder_ ? decoder_->format() : AudioFormat{}),
      total_frames_(decoder_ ? decoder_->total_frames() : 0),
      ring_(config.ring_frames, format_.IsValid() ? format_.channels : 1) {
  if (!decoder_) {
    throw std::invalid_argument("TrackStream: a decoder is required");
  }
  if (!format_.IsValid()) {
    throw std::invalid_argument("TrackStream: the decoder reported no usable format");
  }
  if (config_.decode_chunk_frames == 0) {
    throw std::invalid_argument("TrackStream: decode_chunk_frames must be positive");
  }

  // Allocated here and never again. Read() runs in steady state without
  // touching the allocator, because it must not.
  decode_scratch_.resize(config_.decode_chunk_frames * format_.SamplesPerFrame());
}

TrackStream::~TrackStream() = default;

std::size_t TrackStream::DecodeStep() {
  if (exhausted_.load(std::memory_order_relaxed)) {
    return 0;
  }

  const std::size_t writable = ring_.WritableFrames();
  if (writable == 0) {
    return 0;
  }

  const std::size_t wanted = std::min(writable, config_.decode_chunk_frames);
  const std::size_t decoded = decoder_->Read(decode_scratch_.data(), wanted);
  if (decoded == 0) {
    // release pairs with the acquire in exhausted(): by the time the render
    // thread can see "exhausted", the frames written just before it are
    // visible too.
    exhausted_.store(true, std::memory_order_release);
    return 0;
  }

  const std::size_t written = ring_.Write(decode_scratch_.data(), decoded);
  frames_decoded_.fetch_add(written, std::memory_order_relaxed);
  return written;
}

std::size_t TrackStream::Prime() {
  while (ring_.WritableFrames() > 0) {
    if (DecodeStep() == 0) {
      break;
    }
  }
  return ring_.ReadableFrames();
}

std::size_t TrackStream::Read(float* output, std::size_t frames) noexcept {
  const std::size_t got = ring_.Read(output, frames);
  if (got > 0) {
    frames_read_.fetch_add(got, std::memory_order_relaxed);
  }
  return got;
}

bool TrackStream::SeekFrame(std::uint64_t frame) {
  if (!decoder_->Seek(frame)) {
    return false;
  }
  // The ring still holds audio from before the seek. Dropping it is the whole
  // point: keeping it would play a second of the old position after the user
  // asked to be somewhere else.
  ring_.Reset();
  exhausted_.store(false, std::memory_order_release);
  frames_read_.store(frame, std::memory_order_relaxed);
  frames_decoded_.store(frame, std::memory_order_relaxed);
  return true;
}

}  // namespace sonora::audio
