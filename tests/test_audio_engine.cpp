#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <vector>

#include <sonora/audio/engine.h>

using namespace sonora::audio;

namespace {

// A source with no file behind it. Frame n holds the value n, so the tests can
// check what came out and not merely how much.
//
// This is what the engine's split into DecodeStep() and Render() buys: no
// sound card, no timing, no sleeping. Every test below is deterministic and
// runs in microseconds on all three CI platforms.
class RampDecoder final : public Decoder {
 public:
  RampDecoder(std::uint64_t frames, AudioFormat format) : total_(frames), format_(format) {}

  [[nodiscard]] AudioFormat format() const noexcept override { return format_; }
  [[nodiscard]] std::uint64_t total_frames() const noexcept override { return total_; }

  std::size_t Read(float* output, std::size_t frames) override {
    const std::size_t remaining = static_cast<std::size_t>(total_ - position_);
    const std::size_t count = std::min(frames, remaining);
    for (std::size_t frame = 0; frame < count; ++frame) {
      for (int channel = 0; channel < format_.channels; ++channel) {
        output[frame * format_.SamplesPerFrame() + static_cast<std::size_t>(channel)] =
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

AudioEngine::Config SmallConfig() {
  AudioEngine::Config config;
  config.ring_frames = 512;
  config.decode_chunk_frames = 64;
  config.volume_ramp_ms = 20;
  return config;
}

std::unique_ptr<AudioEngine> MakeEngine(std::uint64_t frames,
                                        AudioEngine::Config config = SmallConfig()) {
  return std::make_unique<AudioEngine>(std::make_unique<RampDecoder>(frames, kStereo48k),
                                       config);
}

}  // namespace

TEST_CASE("the engine reports the source's own format", "[audio][engine]") {
  // Not the device's. Week 5 opens the device at the file's rate rather than
  // resampling, and this is where that decision is visible.
  const auto engine = MakeEngine(1000);
  REQUIRE(engine->format() == kStereo48k);
  REQUIRE(engine->total_frames() == 1000);
}

TEST_CASE("priming fills the buffer before the device starts", "[audio][engine]") {
  auto engine = MakeEngine(10'000);
  const std::size_t buffered = engine->Prime();

  REQUIRE(buffered > 0);
  // And the first callback is therefore not an underrun by construction, which
  // is the entire point of priming.
  std::vector<float> out(256 * 2, -1.0f);
  engine->Render(out.data(), 256);
  REQUIRE(engine->stats().underruns == 0);
  REQUIRE(out[0] == 0.0f);
  REQUIRE(out[2] == 1.0f);  // second frame, first channel
}

TEST_CASE("samples reach the callback unchanged at full volume", "[audio][engine]") {
  auto engine = MakeEngine(10'000);
  engine->Prime();

  std::vector<float> out(128 * 2, 0.0f);
  engine->Render(out.data(), 128);
  for (std::size_t frame = 0; frame < 128; ++frame) {
    REQUIRE(out[frame * 2] == static_cast<float>(frame));
    REQUIRE(out[frame * 2 + 1] == static_cast<float>(frame));
  }
}

TEST_CASE("a callback with nothing decoded is an underrun and is silent", "[audio][engine]") {
  auto engine = MakeEngine(10'000);  // deliberately not primed

  std::vector<float> out(128 * 2, 7.0f);
  engine->Render(out.data(), 128);

  REQUIRE(engine->stats().underruns == 1);
  REQUIRE(engine->stats().frames_missing == 128);
  // Silence, not the stale 7.0 that was in the buffer. Playing back whatever
  // the previous callback left is a stutter you can hear.
  REQUIRE(std::all_of(out.begin(), out.end(), [](float v) { return v == 0.0f; }));
}

TEST_CASE("underruns are counted per callback, not per frame", "[audio][engine]") {
  // One long gap and a hundred short ones are different problems, and a
  // counter that cannot tell them apart describes neither.
  auto engine = MakeEngine(10'000);
  std::vector<float> out(64 * 2, 0.0f);
  for (int i = 0; i < 3; ++i) {
    engine->Render(out.data(), 64);
  }
  REQUIRE(engine->stats().underruns == 3);
  REQUIRE(engine->stats().frames_missing == 192);
}

TEST_CASE("the end of a track is not an underrun", "[audio][engine]") {
  // If it were, every successful playback would report one, and a metric that
  // is never zero is a metric nobody reads.
  auto engine = MakeEngine(100);
  while (engine->DecodeStep() > 0) {
  }
  REQUIRE(engine->DecodeStep() == 0);
  REQUIRE(engine->source_exhausted());

  std::vector<float> out(256 * 2, 0.0f);
  engine->Render(out.data(), 256);  // asks for more than the track holds

  REQUIRE(engine->stats().underruns == 0);
  REQUIRE(engine->finished());
  REQUIRE(out[99 * 2] == 99.0f);
  REQUIRE(out[100 * 2] == 0.0f);  // silence after the last frame
}

TEST_CASE("a track is not finished while audio is still buffered", "[audio][engine]") {
  auto engine = MakeEngine(100);
  engine->Prime();
  REQUIRE(engine->source_exhausted());
  REQUIRE_FALSE(engine->finished());  // decoded, not yet heard

  std::vector<float> out(100 * 2, 0.0f);
  engine->Render(out.data(), 100);
  REQUIRE(engine->finished());
}

TEST_CASE("volume is applied as a ramp, not as a step", "[audio][engine]") {
  // A step in the sample values is a click, and a click is the most audible
  // artefact a player can produce.
  auto engine = MakeEngine(100'000);
  engine->Prime();
  engine->SetVolume(0.0f);

  std::vector<float> out(64 * 2, 0.0f);
  engine->Render(out.data(), 64);

  // 20 ms at 48 kHz is 960 frames, so 64 frames in the gain is still most of
  // the way up: the first frames are near their original value and the last
  // are measurably lower, and nothing jumped.
  REQUIRE(out[0] == 0.0f);  // frame 0 holds the value 0, whatever the gain
  const float gain_at_10 = out[10 * 2] / 10.0f;
  const float gain_at_60 = out[60 * 2] / 60.0f;
  REQUIRE(gain_at_10 < 1.0f);
  REQUIRE(gain_at_60 < gain_at_10);
  REQUIRE(gain_at_60 > 0.0f);  // not there yet after 64 of 960 frames
}

TEST_CASE("the ramp reaches the target and stays there", "[audio][engine]") {
  auto engine = MakeEngine(100'000);
  engine->Prime();
  engine->SetVolume(0.0f);

  std::vector<float> out(1024 * 2, 0.0f);
  engine->Render(out.data(), 1024);  // longer than the 960-frame ramp
  REQUIRE(out[1023 * 2] == 0.0f);

  for (int i = 0; i < 4; ++i) {
    engine->DecodeStep();
  }
  engine->Render(out.data(), 64);
  REQUIRE(std::all_of(out.begin(), out.begin() + 128, [](float v) { return v == 0.0f; }));
}

TEST_CASE("volume is clamped to what a gain can be", "[audio][engine]") {
  auto engine = MakeEngine(100);
  engine->SetVolume(-1.0f);
  REQUIRE(engine->volume() == 0.0f);
  engine->SetVolume(17.0f);
  REQUIRE(engine->volume() == 1.0f);
}

TEST_CASE("decoding stops at the end and says which kind of nothing it is", "[audio][engine]") {
  auto engine = MakeEngine(200);

  std::size_t decoded = 0;
  while (const std::size_t step = engine->DecodeStep()) {
    decoded += step;
  }
  REQUIRE(decoded == 200);
  REQUIRE(engine->stats().frames_decoded == 200);
  REQUIRE(engine->source_exhausted());

  // A full ring is also "0 decoded", and the two have to be distinguishable:
  // one means wait, the other means stop.
  auto full = MakeEngine(100'000);
  full->Prime();
  REQUIRE(full->DecodeStep() == 0);
  REQUIRE_FALSE(full->source_exhausted());
}

TEST_CASE("the engine refuses to be built without a usable source", "[audio][engine]") {
  REQUIRE_THROWS(AudioEngine(nullptr, SmallConfig()));
  REQUIRE_THROWS(
      AudioEngine(std::make_unique<RampDecoder>(10, AudioFormat{0, 0}), SmallConfig()));

  AudioEngine::Config bad = SmallConfig();
  bad.decode_chunk_frames = 0;
  REQUIRE_THROWS(AudioEngine(std::make_unique<RampDecoder>(10, kStereo48k), bad));
}
