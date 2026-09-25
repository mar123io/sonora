#include "play_command.h"

#include <sonora/audio/codecs.h>
#include <sonora/audio/decode_thread.h>
#include <sonora/audio/player.h>
#include <sonora/platform/audio_device.h>

#include <chrono>
#include <cstdio>
#include <thread>

namespace sonora::shell {
namespace {

// Half a second of audio ahead of the device, per track. Long enough that the
// decode thread can lose a scheduling quantum without being heard, short
// enough that a seek does not feel sticky.
constexpr int kBufferMs = 500;

constexpr auto kProgressInterval = std::chrono::milliseconds(250);

void PrintProgress(const audio::Player::Snapshot& snapshot, int queue_size) {
  const std::int64_t played = snapshot.position_ms;
  const std::int64_t total = snapshot.duration_ms;
  std::printf("\r  [%d/%d] %3lld:%02lld / %3lld:%02lld   joins: %llu   underruns: %llu   ",
              snapshot.track_index + 1, queue_size, static_cast<long long>(played / 60000),
              static_cast<long long>((played / 1000) % 60),
              static_cast<long long>(total / 60000),
              static_cast<long long>((total / 1000) % 60),
              static_cast<unsigned long long>(snapshot.track_changes),
              static_cast<unsigned long long>(snapshot.underruns));
  std::fflush(stdout);
}

}  // namespace

int RunPlayCommand(const std::vector<std::filesystem::path>& paths) {
  if (paths.empty()) {
    std::fprintf(stderr, "sonora: --play needs at least one file\n");
    return 2;
  }

  // The device's format is decided by the first track and then fixed, and
  // every later track is decoded into it. That is what makes the queue gapless:
  // a device that had to be reopened between two tracks at different rates
  // would produce exactly the hole this is meant to remove.
  audio::AudioFormat device_format;
  try {
    const auto probe = audio::OpenFileDecoder(paths.front());
    device_format = probe->format();
  } catch (const audio::DecoderError& error) {
    std::fprintf(stderr, "sonora: %s\n", error.what());
    return 2;
  }

  audio::Player::Config config;
  config.device_format = device_format;
  config.ring_frames = static_cast<std::size_t>(
      audio::MillisecondsToFrames(kBufferMs, device_format.sample_rate_hz));
  audio::Player player(config);

  for (const auto& path : paths) {
    player.Enqueue(path);
  }
  player.Play();

  // Pumped here, before the device exists, so the first track is decoded and
  // the first callback is not an underrun by construction.
  while (player.Pump()) {
  }

  auto device = platform::CreateAudioDevice();
  const bool started = device->Start(
      platform::AudioDeviceFormat{device_format.sample_rate_hz, device_format.channels},
      [](float* output, std::size_t frames, void* user_data) {
        // The whole real-time path. See
        // docs/adr/0006-the-audio-callback-is-real-time.md.
        static_cast<audio::Player*>(user_data)->Render(output, frames);
      },
      &player);

  if (!started) {
    std::fprintf(stderr, "sonora: no audio device could be opened\n");
    return 1;
  }

  const platform::AudioDeviceFormat opened = device->format();
  std::printf("playing %zu track(s)\n", paths.size());
  std::printf("  device: %s, %d Hz, %d channels, %zu frames per callback\n",
              device->description().c_str(), opened.sample_rate_hz, opened.channels,
              device->buffer_frames());

  {
    audio::DecodeThread pump([&player] { return player.Pump(); }, std::chrono::milliseconds(5));
    for (;;) {
      const auto snapshot = player.snapshot();
      if (snapshot.state == sonora::core::PlaybackState::kStopped) {
        break;
      }
      PrintProgress(snapshot, static_cast<int>(paths.size()));
      std::this_thread::sleep_for(kProgressInterval);
    }
  }
  device->Stop();

  const auto snapshot = player.snapshot();
  PrintProgress(snapshot, static_cast<int>(paths.size()));
  std::printf("\n");
  std::printf("  %llu gapless join(s), %llu underrun(s)",
              static_cast<unsigned long long>(snapshot.track_changes),
              static_cast<unsigned long long>(snapshot.underruns));
  if (!snapshot.last_error.empty()) {
    std::printf(", last error: %s", snapshot.last_error.c_str());
  }
  std::printf("\n");

  // A join is a claim this command can check: N files should produce N-1
  // joins, and anything less means a track was dropped rather than played.
  const bool joined_all = snapshot.track_changes + 1 == paths.size();
  if (!joined_all) {
    std::fprintf(stderr, "sonora: expected %zu join(s), got %llu\n", paths.size() - 1,
                 static_cast<unsigned long long>(snapshot.track_changes));
  }
  return (snapshot.underruns == 0 && joined_all) ? 0 : 1;
}

}  // namespace sonora::shell
