#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

#include <sonora/audio/ring_buffer.h>

using namespace sonora::audio;

namespace {

// Frame n holds the value n in every channel, so a test can say exactly which
// frames arrived and in what order -- not merely how many.
void FillRamp(std::vector<float>& buffer,
              std::size_t first_frame,
              std::size_t frames,
              int channels) {
  buffer.resize(frames * static_cast<std::size_t>(channels));
  for (std::size_t frame = 0; frame < frames; ++frame) {
    for (int channel = 0; channel < channels; ++channel) {
      buffer[frame * static_cast<std::size_t>(channels) + static_cast<std::size_t>(channel)] =
          static_cast<float>(first_frame + frame);
    }
  }
}

}  // namespace

TEST_CASE("an empty buffer gives nothing", "[audio][ring]") {
  RingBuffer ring(64, 2);
  std::vector<float> out(64 * 2, -1.0f);

  REQUIRE(ring.ReadableFrames() == 0);
  REQUIRE(ring.Read(out.data(), 64) == 0);
  REQUIRE(out[0] == -1.0f);  // untouched, not zeroed
}

TEST_CASE("frames come back in the order they went in", "[audio][ring]") {
  RingBuffer ring(64, 2);
  std::vector<float> in;
  FillRamp(in, 0, 40, 2);

  REQUIRE(ring.Write(in.data(), 40) == 40);
  REQUIRE(ring.ReadableFrames() == 40);

  std::vector<float> out(40 * 2, 0.0f);
  REQUIRE(ring.Read(out.data(), 40) == 40);
  REQUIRE(out == in);
  REQUIRE(ring.ReadableFrames() == 0);
}

TEST_CASE("capacity is rounded up and one frame is held back", "[audio][ring]") {
  // Rounded to a power of two so the wrap is a mask; one frame short of it so
  // full and empty can be told apart without a separate flag.
  RingBuffer ring(100, 1);
  REQUIRE(ring.capacity_frames() == 127);
  REQUIRE(ring.WritableFrames() == 127);
}

TEST_CASE("a full buffer takes what fits and says so", "[audio][ring]") {
  // A request is a minimum, not an exact size: asking for 16 gives 31, because
  // the storage is rounded up to a power of two and one frame is held back.
  // Asserting against capacity_frames() rather than against the number that
  // was asked for is the difference between testing the contract and testing
  // an arithmetic coincidence.
  RingBuffer ring(16, 1);
  const std::size_t capacity = ring.capacity_frames();
  REQUIRE(capacity >= 16);

  std::vector<float> in;
  FillRamp(in, 0, capacity + 16, 1);

  REQUIRE(ring.Write(in.data(), capacity + 16) == capacity);
  REQUIRE(ring.WritableFrames() == 0);
  REQUIRE(ring.Write(in.data(), 1) == 0);

  std::vector<float> out(capacity, 0.0f);
  REQUIRE(ring.Read(out.data(), capacity) == capacity);
  REQUIRE(out.front() == 0.0f);
  REQUIRE(out.back() == static_cast<float>(capacity - 1));
}

TEST_CASE("the indices wrap without losing or duplicating a frame", "[audio][ring]") {
  // The interesting case is a read or a write that straddles the end of the
  // storage: it is two memcpys, and getting the split wrong produces audio
  // that is subtly wrong rather than obviously broken.
  constexpr int kChannels = 2;
  RingBuffer ring(16, kChannels);

  std::size_t next_written = 0;
  std::size_t next_expected = 0;
  std::vector<float> in;
  std::vector<float> out(16 * kChannels, 0.0f);

  for (int round = 0; round < 50; ++round) {
    const std::size_t chunk = 5;  // never a divisor of the 16-frame capacity
    FillRamp(in, next_written, chunk, kChannels);
    REQUIRE(ring.Write(in.data(), chunk) == chunk);
    next_written += chunk;

    REQUIRE(ring.Read(out.data(), chunk) == chunk);
    for (std::size_t frame = 0; frame < chunk; ++frame) {
      REQUIRE(out[frame * kChannels] == static_cast<float>(next_expected + frame));
      REQUIRE(out[frame * kChannels + 1] == static_cast<float>(next_expected + frame));
    }
    next_expected += chunk;
  }
  REQUIRE(next_expected == 250);
}

TEST_CASE("a short read leaves the rest of the destination alone", "[audio][ring]") {
  // The engine relies on this: it fills the remainder with silence itself,
  // and would have no way to know where to start if a short read scribbled.
  RingBuffer ring(64, 1);
  std::vector<float> in;
  FillRamp(in, 0, 3, 1);
  ring.Write(in.data(), 3);

  std::vector<float> out(10, -1.0f);
  REQUIRE(ring.Read(out.data(), 10) == 3);
  REQUIRE(out[2] == 2.0f);
  REQUIRE(out[3] == -1.0f);
}

TEST_CASE("a producer and a consumer on two threads lose nothing", "[audio][ring]") {
  // The reason this class exists. Everything above tests the arithmetic; this
  // tests the memory ordering, which is the part that works on one thread and
  // fails on a machine with more cores than the author's.
  constexpr int kChannels = 2;
  constexpr std::size_t kTotalFrames = 200'000;
  RingBuffer ring(1024, kChannels);

  std::atomic<bool> producer_done{false};

  std::thread producer([&] {
    std::vector<float> chunk;
    std::size_t sent = 0;
    while (sent < kTotalFrames) {
      const std::size_t want = std::min<std::size_t>(97, kTotalFrames - sent);
      FillRamp(chunk, sent, want, kChannels);
      std::size_t offset = 0;
      while (offset < want) {
        const std::size_t written =
            ring.Write(chunk.data() + offset * kChannels, want - offset);
        offset += written;
        if (written == 0) {
          std::this_thread::yield();
        }
      }
      sent += want;
    }
    producer_done.store(true, std::memory_order_release);
  });

  std::size_t received = 0;
  bool in_order = true;
  std::vector<float> out(256 * kChannels, 0.0f);
  while (received < kTotalFrames) {
    const std::size_t read = ring.Read(out.data(), 256);
    if (read == 0) {
      if (producer_done.load(std::memory_order_acquire) && ring.ReadableFrames() == 0 &&
          received >= kTotalFrames) {
        break;
      }
      std::this_thread::yield();
      continue;
    }
    for (std::size_t frame = 0; frame < read; ++frame) {
      if (out[frame * kChannels] != static_cast<float>(received + frame) ||
          out[frame * kChannels + 1] != static_cast<float>(received + frame)) {
        in_order = false;
      }
    }
    received += read;
  }

  producer.join();
  REQUIRE(in_order);
  REQUIRE(received == kTotalFrames);
}

TEST_CASE("the buffer refuses configurations that cannot work", "[audio][ring]") {
  REQUIRE_THROWS(RingBuffer(0, 2));
  REQUIRE_THROWS(RingBuffer(64, 0));
}
