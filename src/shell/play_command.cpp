#include "play_command.h"

#include <sonora/audio/codecs.h>
#include <sonora/audio/decode_thread.h>
#include <sonora/audio/engine.h>
#include <sonora/platform/audio_device.h>

#include <chrono>
#include <cstdio>
#include <thread>

namespace sonora::shell {
namespace {

// Half a second of audio ahead of the device. Long enough that the decode
// thread can lose a scheduling quantum without being heard, short enough that
// a seek will not feel sticky when week 6 adds one.
constexpr int kBufferMs = 500;

// How often the progress line is rewritten. Not a timing mechanism -- the
// device decides when audio happens; this only decides when to print.
constexpr auto kProgressInterval = std::chrono::milliseconds(250);

void PrintProgress(const audio::AudioEngine& engine, int sample_rate_hz) {
  const auto stats = engine.stats();
  const std::int64_t played_ms =
      audio::FramesToMilliseconds(stats.frames_rendered, sample_rate_hz);
  const std::int64_t total_ms =
      audio::FramesToMilliseconds(engine.total_frames(), sample_rate_hz);

  std::printf("\r  %3lld:%02lld / %3lld:%02lld   underruns: %llu   ",
              static_cast<long long>(played_ms / 60000),
              static_cast<long long>((played_ms / 1000) % 60),
              static_cast<long long>(total_ms / 60000),
              static_cast<long long>((total_ms / 1000) % 60),
              static_cast<unsigned long long>(stats.underruns));
  std::fflush(stdout);
}

}  // namespace

int RunPlayCommand(const std::filesystem::path& path) {
  audio::DecoderPtr decoder;
  try {
    decoder = audio::OpenFileDecoder(path);
  } catch (const audio::DecoderError& error) {
    std::fprintf(stderr, "sonora: %s\n", error.what());
    return 2;
  }

  const audio::AudioFormat format = decoder->format();
  const std::uint64_t total_frames = decoder->total_frames();
  std::printf("playing %s\n", path.filename().string().c_str());
  std::printf(
      "  source: %d Hz, %d channels, %lld ms\n", format.sample_rate_hz, format.channels,
      static_cast<long long>(audio::FramesToMilliseconds(total_frames, format.sample_rate_hz)));

  audio::AudioEngine::Config config;
  config.ring_frames =
      static_cast<std::size_t>(audio::MillisecondsToFrames(kBufferMs, format.sample_rate_hz));
  audio::AudioEngine engine(std::move(decoder), config);

  // Fills the ring before the device exists, so the very first callback has
  // something to play. Without this every run would report one underrun that
  // says nothing about the machine.
  engine.Prime();

  auto device = platform::CreateAudioDevice();

  // The device is asked for the *source's* format rather than a fixed 48 kHz.
  // Week 5 owns no resampler, and the operating system's mixer already has a
  // good one; opening at the file's own rate means nothing here has to
  // resample, and a 44.1 kHz file does not play a semitone sharp. Week 6 needs
  // a real resampler anyway, because gapless playback across two files at
  // different rates cannot reopen the device between them.
  const bool started = device->Start(
      platform::AudioDeviceFormat{format.sample_rate_hz, format.channels},
      [](float* output, std::size_t frames, void* user_data) {
        // The whole real-time path, in one line: no allocation, no lock, no
        // I/O, no logging. See docs/adr/0006-the-audio-callback-is-real-time.md.
        static_cast<audio::AudioEngine*>(user_data)->Render(output, frames);
      },
      &engine);

  if (!started) {
    std::fprintf(stderr, "sonora: no audio device could be opened\n");
    return 1;
  }

  const platform::AudioDeviceFormat device_format = device->format();
  std::printf("  device: %s, %d Hz, %d channels, %zu frames per callback\n",
              device->description().c_str(), device_format.sample_rate_hz,
              device_format.channels, device->buffer_frames());

  {
    // Started after the device, stopped before it: the pump borrows the engine
    // and so does the callback, and the engine has to outlive both.
    audio::DecodeThread pump(engine, std::chrono::milliseconds(5));
    while (!engine.finished()) {
      std::this_thread::sleep_for(kProgressInterval);
      PrintProgress(engine, format.sample_rate_hz);
    }
  }
  device->Stop();

  const audio::AudioEngine::Stats stats = engine.stats();
  PrintProgress(engine, format.sample_rate_hz);
  std::printf("\n");
  std::printf("  decoded %llu frames, rendered %llu, %llu underrun(s)",
              static_cast<unsigned long long>(stats.frames_decoded),
              static_cast<unsigned long long>(stats.frames_rendered),
              static_cast<unsigned long long>(stats.underruns));
  if (stats.underruns > 0) {
    std::printf(", %lld ms of silence inserted",
                static_cast<long long>(
                    audio::FramesToMilliseconds(stats.frames_missing, format.sample_rate_hz)));
  }
  std::printf("\n");

  // A non-zero exit on an underrun, so this is usable as a check and not only
  // as something to watch.
  return stats.underruns == 0 ? 0 : 1;
}

}  // namespace sonora::shell
