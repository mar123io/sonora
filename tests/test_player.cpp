#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <sonora/audio/player.h>

using namespace sonora::audio;
using sonora::core::PlaybackState;

namespace {

constexpr AudioFormat kDevice{48000, 2};

// A track whose frame n holds the value `first_value + n`, in every channel.
//
// Two tracks laid end to end -- one running 0..999, the next 1000..1999 --
// make the gapless join checkable without listening to anything: the rendered
// output has to be one unbroken ramp. A gap shows up as a zero in the middle,
// a repeat as a value that appears twice, and a skip as a number missing. None
// of those need a sound card or a stopwatch to find.
class RampDecoder final : public Decoder {
 public:
  RampDecoder(double first_value, std::uint64_t frames, AudioFormat format)
      : first_value_(first_value), total_(frames), format_(format) {}

  [[nodiscard]] AudioFormat format() const noexcept override { return format_; }
  [[nodiscard]] std::uint64_t total_frames() const noexcept override { return total_; }

  std::size_t Read(float* output, std::size_t frames) override {
    const std::size_t count = static_cast<std::size_t>(
        std::min<std::uint64_t>(frames, total_ > position_ ? total_ - position_ : 0));
    for (std::size_t frame = 0; frame < count; ++frame) {
      const float value =
          static_cast<float>(first_value_ + static_cast<double>(position_ + frame));
      for (std::size_t channel = 0; channel < format_.SamplesPerFrame(); ++channel) {
        output[frame * format_.SamplesPerFrame() + channel] = value;
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
  double first_value_;
  std::uint64_t total_;
  std::uint64_t position_ = 0;
  AudioFormat format_;
};

// Stands in for a sound card: it consumes buffers when the test says so, in
// whatever sizes the test chooses, and time only passes because the test says
// it did.
class Harness {
 public:
  struct Track {
    double first_value = 0.0;
    std::uint64_t frames = 0;
  };

  explicit Harness(std::map<std::string, Track> tracks, Player::Config config = {})
      : tracks_(std::move(tracks)) {
    config.device_format = kDevice;
    config.ring_frames = 4096;
    config.preload_ahead_ms = 5000;
    config.open_track = [this](const std::filesystem::path& path, AudioFormat target) {
      const auto found = tracks_.find(path.string());
      if (found == tracks_.end()) {
        throw DecoderError("no such fake track: " + path.string());
      }
      return std::unique_ptr<Decoder>(std::make_unique<RampDecoder>(
          found->second.first_value, found->second.frames, target));
    };
    player_ = std::make_unique<Player>(config);
  }

  Player& player() { return *player_; }

  // Pumps until there is nothing left to do, so a test never depends on how
  // many times the decode thread happened to run.
  void PumpUntilIdle(int limit = 500) {
    for (int i = 0; i < limit; ++i) {
      if (!player_->Pump()) {
        return;
      }
    }
  }

  // One callback. Appends what came out, so the whole session can be checked
  // as a single stream afterwards.
  void Render(std::size_t frames) {
    buffer_.assign(frames * kDevice.SamplesPerFrame(), -1.0f);
    player_->Render(buffer_.data(), frames);
    output_.insert(output_.end(), buffer_.begin(), buffer_.end());
  }

  // Alternates pumping and rendering, the way the two threads really do.
  void Run(std::size_t callbacks, std::size_t frames_per_callback) {
    for (std::size_t i = 0; i < callbacks; ++i) {
      PumpUntilIdle();
      Render(frames_per_callback);
    }
  }

  [[nodiscard]] float frame(std::size_t index) const {
    return output_[index * kDevice.SamplesPerFrame()];
  }

  [[nodiscard]] std::size_t rendered_frames() const {
    return output_.size() / kDevice.SamplesPerFrame();
  }

 private:
  std::map<std::string, Track> tracks_;
  std::unique_ptr<Player> player_;
  std::vector<float> buffer_;
  std::vector<float> output_;
};

}  // namespace

TEST_CASE("a new player is idle and silent", "[audio][player]") {
  Harness harness({});
  harness.Render(64);

  const auto snapshot = harness.player().snapshot();
  REQUIRE(snapshot.state == PlaybackState::kIdle);
  REQUIRE(snapshot.track_index == -1);
  REQUIRE(harness.frame(0) == 0.0f);
  REQUIRE(snapshot.underruns == 0);  // nothing to be late for
}

TEST_CASE("playing a queued track reaches the callback", "[audio][player]") {
  Harness harness({{"a", {0.0, 5000}}});
  harness.player().Enqueue("a");
  harness.player().Play();
  harness.PumpUntilIdle();

  harness.Render(128);
  REQUIRE(harness.player().snapshot().state == PlaybackState::kPlaying);
  REQUIRE(harness.frame(0) == 0.0f);
  REQUIRE(harness.frame(100) == 100.0f);
}

TEST_CASE("two tracks join with no gap, no repeat and no skip", "[audio][player][gapless]") {
  // The week's whole point, stated as an assertion. Track "a" runs 0..999 and
  // track "b" runs 1000..1999, so a correct join is one unbroken ramp from 0
  // to 1999 and any fault in the handover shows up as a wrong number.
  //
  // 96 frames per callback does not divide 1000, so the join lands in the
  // middle of a buffer -- which is the case that matters. A join that only
  // works on a callback boundary is not gapless, it is lucky.
  Harness harness({{"a", {0.0, 1000}}, {"b", {1000.0, 1000}}});
  harness.player().Enqueue("a");
  harness.player().Enqueue("b");
  harness.player().Play();

  harness.Run(/*callbacks=*/25, /*frames_per_callback=*/96);

  REQUIRE(harness.rendered_frames() == 2400);

  bool unbroken = true;
  std::size_t first_bad = 0;
  for (std::size_t frame = 0; frame < 2000; ++frame) {
    if (harness.frame(frame) != static_cast<float>(frame)) {
      unbroken = false;
      first_bad = frame;
      break;
    }
  }
  INFO("first wrong frame: " << first_bad << " = " << harness.frame(first_bad));
  REQUIRE(unbroken);

  // And after the queue runs out, silence rather than a repeat of the tail.
  REQUIRE(harness.frame(2000) == 0.0f);
  REQUIRE(harness.frame(2399) == 0.0f);

  const auto snapshot = harness.player().snapshot();
  REQUIRE(snapshot.track_changes == 1);
  REQUIRE(snapshot.underruns == 0);
  REQUIRE(snapshot.state == PlaybackState::kStopped);
}

TEST_CASE("the join survives a callback size that lands exactly on it",
          "[audio][player][gapless]") {
  // The other case: the track ends precisely at a buffer boundary. It is the
  // one that works by accident in a broken implementation, so it is worth
  // pinning separately rather than trusting the awkward one to cover it.
  Harness harness({{"a", {0.0, 1024}}, {"b", {1024.0, 1024}}});
  harness.player().Enqueue("a");
  harness.player().Enqueue("b");
  harness.player().Play();

  harness.Run(/*callbacks=*/10, /*frames_per_callback=*/256);

  for (std::size_t frame = 0; frame < 2048; ++frame) {
    REQUIRE(harness.frame(frame) == static_cast<float>(frame));
  }
  REQUIRE(harness.player().snapshot().track_changes == 1);
}

TEST_CASE("three tracks in a row make two joins", "[audio][player][gapless]") {
  Harness harness({{"a", {0.0, 700}}, {"b", {700.0, 700}}, {"c", {1400.0, 700}}});
  for (const char* name : {"a", "b", "c"}) {
    harness.player().Enqueue(name);
  }
  harness.player().Play();
  harness.Run(/*callbacks=*/30, /*frames_per_callback=*/96);

  for (std::size_t frame = 0; frame < 2100; ++frame) {
    REQUIRE(harness.frame(frame) == static_cast<float>(frame));
  }
  REQUIRE(harness.player().snapshot().track_changes == 2);
}

TEST_CASE("a late decode thread is an underrun, the end of the queue is not",
          "[audio][player]") {
  Harness harness({{"a", {0.0, 400}}});
  harness.player().Enqueue("a");
  harness.player().Play();
  harness.PumpUntilIdle();

  // Rendering far more than was decoded, without pumping in between.
  harness.Render(8192);
  const std::uint64_t after_starvation = harness.player().snapshot().underruns;
  REQUIRE(after_starvation == 0);  // the track simply ended; nothing was late

  Harness busy({{"b", {0.0, 1'000'000}}});
  busy.player().Enqueue("b");
  busy.player().Play();
  busy.PumpUntilIdle();
  busy.Render(8192);  // more than the 4096-frame ring can hold
  REQUIRE(busy.player().snapshot().underruns == 1);
}

TEST_CASE("pause fades out and then stops consuming", "[audio][player]") {
  // Cutting to zero on the frame the button was pressed is a click. Fading and
  // then stopping means the frames that were consumed are the frames that were
  // heard, quieter -- which is what a fade-out is.
  Harness harness({{"a", {1000.0, 100'000}}});
  harness.player().Enqueue("a");
  harness.player().Play();
  harness.Run(4, 256);

  const std::int64_t before = harness.player().snapshot().position_ms;
  harness.player().Pause();

  harness.Run(4, 256);
  const std::int64_t during = harness.player().snapshot().position_ms;
  REQUIRE(during > before);  // the fade consumed a little

  harness.Run(4, 256);
  REQUIRE(harness.player().snapshot().position_ms == during);  // and then stopped
  REQUIRE(harness.player().snapshot().state == PlaybackState::kPaused);

  harness.player().Play();
  harness.Run(4, 256);
  REQUIRE(harness.player().snapshot().position_ms > during);
  REQUIRE(harness.player().snapshot().state == PlaybackState::kPlaying);
}

TEST_CASE("volume is a ramp and is clamped", "[audio][player]") {
  Harness harness({{"a", {1.0, 100'000}}});
  harness.player().Enqueue("a");
  harness.player().Play();
  harness.player().SetVolume(17.0f);
  REQUIRE(harness.player().snapshot().volume == 1.0f);
  harness.player().SetVolume(-3.0f);
  REQUIRE(harness.player().snapshot().volume == 0.0f);

  harness.player().SetVolume(0.0f);
  harness.Run(1, 256);

  // The source itself ramps -- frame n holds the value 1 + n -- so comparing
  // two output samples says nothing about the gain. Dividing by what the frame
  // would have been at full volume is what isolates it.
  const float gain_at_0 = harness.frame(0) / 1.0f;
  const float gain_at_255 = harness.frame(255) / 256.0f;

  // 20 ms at 48 kHz is 960 frames, so 256 frames in the gain is partway down
  // and nothing jumped.
  REQUIRE(gain_at_0 < 1.0f);
  REQUIRE(gain_at_255 < gain_at_0);
  REQUIRE(gain_at_255 > 0.0f);
}

TEST_CASE("seeking moves the position and keeps playing", "[audio][player]") {
  Harness harness({{"a", {0.0, 480'000}}});  // ten seconds at 48 kHz
  harness.player().Enqueue("a");
  harness.player().Play();
  harness.Run(4, 256);

  harness.player().SeekMs(5000);
  harness.PumpUntilIdle();
  harness.Render(256);

  const auto snapshot = harness.player().snapshot();
  REQUIRE(snapshot.position_ms >= 5000);
  REQUIRE(snapshot.position_ms < 5100);
  REQUIRE(snapshot.duration_ms == 10000);
}

TEST_CASE("next and previous move through the queue", "[audio][player]") {
  Harness harness({{"a", {0.0, 480'000}}, {"b", {1'000'000.0, 480'000}}});
  harness.player().Enqueue("a");
  harness.player().Enqueue("b");
  harness.player().Play();
  harness.Run(2, 256);
  REQUIRE(harness.player().snapshot().track_index == 0);

  harness.player().Next();
  harness.PumpUntilIdle();
  harness.Render(256);
  REQUIRE(harness.player().snapshot().track_index == 1);

  // Early in a track, "previous" means the previous track...
  harness.player().Previous();
  harness.PumpUntilIdle();
  harness.Render(256);
  REQUIRE(harness.player().snapshot().track_index == 0);

  // ...and later in a track it means the start of this one, which is what
  // every player does and what people expect.
  harness.Run(80, 4096);  // past the three-second mark
  REQUIRE(harness.player().snapshot().position_ms > 3000);
  harness.player().Previous();
  harness.PumpUntilIdle();
  harness.Render(256);
  REQUIRE(harness.player().snapshot().track_index == 0);
  REQUIRE(harness.player().snapshot().position_ms < 1000);
}

TEST_CASE("the queue version changes when the contents do", "[audio][player]") {
  // The number the interface watches. It exists because the two numbers a page
  // would reach for first -- how many tracks are queued, and which one is
  // playing -- are both blind to the commonest change there is: playing one
  // track and then playing another. One entry before, one entry after, index 0
  // both times, completely different music.
  Harness harness({{"a", {0.0, 2000}}, {"b", {1000.0, 2000}}});

  const std::uint64_t empty = harness.player().snapshot().queue_version;

  harness.player().Enqueue("a");
  harness.PumpUntilIdle();
  const std::uint64_t one = harness.player().snapshot().queue_version;
  REQUIRE(one != empty);

  // Replace: clear then enqueue, which is what player.enqueue with replace does.
  harness.player().ClearQueue();
  harness.player().Enqueue("b");
  harness.PumpUntilIdle();
  const auto after = harness.player().snapshot();

  REQUIRE(after.queue_size == 1);       // same size...
  REQUIRE(after.queue_version != one);  // ...and the page can still tell.

  // A command that does not touch the queue leaves it alone: a version that
  // moved on every event would be a version nobody could use to skip work.
  harness.player().Play();
  harness.player().SetVolume(0.5f);
  harness.PumpUntilIdle();
  REQUIRE(harness.player().snapshot().queue_version == after.queue_version);
}

TEST_CASE("a jump plays the track it names, from anywhere", "[audio][player]") {
  // What clicking a row of the queue does. Unlike next and previous it is
  // absolute, so it also has to work when nothing is playing -- which is the
  // state the queue panel is in right after a scan, before anyone has pressed
  // play.
  Harness harness({{"a", {0.0, 2000}}, {"b", {1000.0, 2000}}, {"c", {2000.0, 2000}}});
  harness.player().Enqueue("a");
  harness.player().Enqueue("b");
  harness.player().Enqueue("c");

  // From idle, with no Play() first.
  harness.player().PlayTrack(2);
  harness.PumpUntilIdle();
  harness.Render(64);
  REQUIRE(harness.player().snapshot().state == PlaybackState::kPlaying);
  REQUIRE(harness.player().snapshot().track_index == 2);
  REQUIRE(harness.frame(0) == 2000.0f);

  // And backwards, mid-playback.
  harness.player().PlayTrack(0);
  harness.PumpUntilIdle();
  const std::size_t before = harness.rendered_frames();
  harness.Render(64);
  REQUIRE(harness.player().snapshot().track_index == 0);
  REQUIRE(harness.frame(before) == 0.0f);

  // An index the queue does not have is ignored rather than being an error: the
  // queue can change between the page drawing a row and somebody clicking it.
  harness.player().PlayTrack(99);
  harness.player().PlayTrack(-1);
  harness.PumpUntilIdle();
  REQUIRE(harness.player().snapshot().track_index == 0);
  REQUIRE(harness.player().snapshot().state == PlaybackState::kPlaying);
}

TEST_CASE("next past the end of the queue changes nothing", "[audio][player]") {
  Harness harness({{"a", {0.0, 480'000}}});
  harness.player().Enqueue("a");
  harness.player().Play();
  harness.Run(2, 256);

  harness.player().Next();
  harness.PumpUntilIdle();
  harness.Render(256);
  REQUIRE(harness.player().snapshot().track_index == 0);
  REQUIRE(harness.player().snapshot().state == PlaybackState::kPlaying);
}

TEST_CASE("stop lets go of the track and goes quiet", "[audio][player]") {
  Harness harness({{"a", {5.0, 480'000}}});
  harness.player().Enqueue("a");
  harness.player().Play();
  harness.Run(2, 256);

  harness.player().Stop();
  harness.PumpUntilIdle();
  harness.Render(256);

  const auto snapshot = harness.player().snapshot();
  REQUIRE(snapshot.state == PlaybackState::kStopped);
  REQUIRE(snapshot.track_index == -1);
  REQUIRE(harness.frame(harness.rendered_frames() - 1) == 0.0f);
}

TEST_CASE("a track that cannot be opened is reported, not fatal", "[audio][player]") {
  Harness harness({{"a", {0.0, 1000}}});
  harness.player().Enqueue("missing");
  harness.player().Play();
  harness.PumpUntilIdle();
  harness.Render(256);

  const auto snapshot = harness.player().snapshot();
  REQUIRE(snapshot.state == PlaybackState::kStopped);
  REQUIRE(snapshot.last_error.find("missing") != std::string::npos);
}

TEST_CASE("an unreadable track in the middle does not stop the queue",
          "[audio][player][gapless]") {
  // The preload finds the bad file while the current track is still playing,
  // which is the good case: there is time to notice, and the failure costs the
  // one track rather than the session.
  Harness harness({{"a", {0.0, 1000}}});
  harness.player().Enqueue("a");
  harness.player().Enqueue("broken");
  harness.player().Play();
  harness.Run(20, 96);

  const auto snapshot = harness.player().snapshot();
  REQUIRE(snapshot.last_error.find("broken") != std::string::npos);
  REQUIRE(snapshot.track_changes == 0);
  REQUIRE(snapshot.state == PlaybackState::kStopped);
  for (std::size_t frame = 0; frame < 1000; ++frame) {
    REQUIRE(harness.frame(frame) == static_cast<float>(frame));  // "a" still played in full
  }
}

TEST_CASE("the player refuses to be built without a device format", "[audio][player]") {
  Player::Config config;
  REQUIRE_THROWS(Player(config));
}
