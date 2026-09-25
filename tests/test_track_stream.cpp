#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <vector>

#include <sonora/audio/track_stream.h>

using namespace sonora::audio;

namespace {

// Frame n holds the value n, so a test can say which frames arrived and in
// what order rather than only how many.
class RampDecoder final : public Decoder {
 public:
  RampDecoder(std::uint64_t frames, AudioFormat format) : total_(frames), format_(format) {}

  [[nodiscard]] AudioFormat format() const noexcept override { return format_; }
  [[nodiscard]] std::uint64_t total_frames() const noexcept override { return total_; }

  std::size_t Read(float* output, std::size_t frames) override {
    const std::size_t count = static_cast<std::size_t>(
        std::min<std::uint64_t>(frames, total_ > position_ ? total_ - position_ : 0));
    for (std::size_t frame = 0; frame < count; ++frame) {
      for (std::size_t channel = 0; channel < format_.SamplesPerFrame(); ++channel) {
        output[frame * format_.SamplesPerFrame() + channel] =
            static_cast<float>(position_ + frame);
      }
    }
    position_ += count;
    return count;
  }

  bool Seek(std::uint64_t frame) override {
    if (frame > total_) {
      return false;
    }
    position_ = frame;
    return true;
  }

 private:
  std::uint64_t total_;
  std::uint64_t position_ = 0;
  AudioFormat format_;
};

constexpr AudioFormat kStereo48k{48000, 2};

TrackStream::Config SmallConfig() {
  TrackStream::Config config;
  config.ring_frames = 512;
  config.decode_chunk_frames = 64;
  return config;
}

std::unique_ptr<TrackStream> MakeStream(std::uint64_t frames,
                                        TrackStream::Config config = SmallConfig()) {
  return std::make_unique<TrackStream>(std::make_unique<RampDecoder>(frames, kStereo48k),
                                       config);
}

}  // namespace

TEST_CASE("a stream reports the decoder's format and length", "[audio][stream]") {
  const auto stream = MakeStream(1000);
  REQUIRE(stream->format() == kStereo48k);
  REQUIRE(stream->total_frames() == 1000);
}

TEST_CASE("priming fills the ring completely", "[audio][stream]") {
  auto stream = MakeStream(100'000);
  REQUIRE(stream->Prime() > 0);
  REQUIRE(stream->DecodeStep() == 0);  // full
  REQUIRE_FALSE(stream->exhausted());  // but not over
}

TEST_CASE("frames come out in order and unchanged", "[audio][stream]") {
  auto stream = MakeStream(10'000);
  stream->Prime();

  std::vector<float> out(128 * 2, -1.0f);
  REQUIRE(stream->Read(out.data(), 128) == 128);
  for (std::size_t frame = 0; frame < 128; ++frame) {
    REQUIRE(out[frame * 2] == static_cast<float>(frame));
    REQUIRE(out[frame * 2 + 1] == static_cast<float>(frame));
  }
  REQUIRE(stream->frames_read() == 128);
}

TEST_CASE("a short read leaves the rest of the buffer alone", "[audio][stream]") {
  // Player fills the remainder, either with the next track or with silence.
  // A stream that zeroed it here would make the gapless join impossible.
  auto stream = MakeStream(100);
  stream->Prime();

  std::vector<float> out(256 * 2, -1.0f);
  REQUIRE(stream->Read(out.data(), 256) == 100);
  REQUIRE(out[99 * 2] == 99.0f);
  REQUIRE(out[100 * 2] == -1.0f);  // untouched
}

TEST_CASE("exhausted and finished are different questions", "[audio][stream]") {
  auto stream = MakeStream(100);
  stream->Prime();
  REQUIRE(stream->exhausted());       // the decoder has no more
  REQUIRE_FALSE(stream->finished());  // but 100 frames are still buffered
  REQUIRE(stream->buffered_frames() == 100);

  std::vector<float> out(100 * 2, 0.0f);
  REQUIRE(stream->Read(out.data(), 100) == 100);
  REQUIRE(stream->finished());
}

TEST_CASE("decoding reports which kind of nothing it found", "[audio][stream]") {
  auto ended = MakeStream(200);
  std::size_t decoded = 0;
  while (const std::size_t step = ended->DecodeStep()) {
    decoded += step;
  }
  REQUIRE(decoded == 200);
  REQUIRE(ended->exhausted());

  auto full = MakeStream(100'000);
  full->Prime();
  REQUIRE(full->DecodeStep() == 0);
  REQUIRE_FALSE(full->exhausted());
}

TEST_CASE("seeking drops what was buffered", "[audio][stream]") {
  // Keeping it would play a second of the old position after the listener
  // asked to be somewhere else.
  auto stream = MakeStream(100'000);
  stream->Prime();
  REQUIRE(stream->buffered_frames() > 0);

  REQUIRE(stream->SeekFrame(50'000));
  REQUIRE(stream->buffered_frames() == 0);
  REQUIRE(stream->frames_read() == 50'000);

  stream->Prime();
  std::vector<float> out(64 * 2, 0.0f);
  REQUIRE(stream->Read(out.data(), 64) == 64);
  REQUIRE(out[0] == 50'000.0f);
}

TEST_CASE("seeking past the end is refused", "[audio][stream]") {
  auto stream = MakeStream(1000);
  REQUIRE_FALSE(stream->SeekFrame(2000));
}

TEST_CASE("a stream refuses a source it cannot use", "[audio][stream]") {
  REQUIRE_THROWS(TrackStream(nullptr, SmallConfig()));
  REQUIRE_THROWS(
      TrackStream(std::make_unique<RampDecoder>(10, AudioFormat{0, 0}), SmallConfig()));

  TrackStream::Config bad = SmallConfig();
  bad.decode_chunk_frames = 0;
  REQUIRE_THROWS(TrackStream(std::make_unique<RampDecoder>(10, kStereo48k), bad));
}
