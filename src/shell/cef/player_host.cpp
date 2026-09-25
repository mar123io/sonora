#include "cef/player_host.h"

#include <sonora/audio/decode_thread.h>
#include <sonora/audio/player.h>
#include <sonora/platform/audio_device.h>

#include <chrono>

namespace sonora::shell {
namespace {

// The shell opens one device, once, at a fixed format, and every track is
// decoded into it. Week 5's --play opened at the file's own rate because it
// played one file; a queue cannot do that, because two tracks at different
// rates would need the device reopened between them -- which is the gap week 6
// exists to remove. 48 kHz stereo is what the mixer runs at on a modern
// desktop, so it is also the conversion the operating system would have done
// anyway.
constexpr audio::AudioFormat kDeviceFormat{48000, 2};

// Half a second of audio ahead of the device, per track.
constexpr int kBufferMs = 500;

// The player is sampled ten times a second and the page is told four times a
// second: the coalescer decides, not this timer. Sampling faster than the
// delivery rate is the point -- it means the value that does arrive is the
// newest one rather than one from up to 250 ms ago.
constexpr std::int64_t kStateSampleMs = 100;

}  // namespace

PlayerHost::PlayerHost() = default;

PlayerHost::~PlayerHost() {
  Stop();
}

bool PlayerHost::Start() {
  audio::Player::Config config;
  config.device_format = kDeviceFormat;
  config.ring_frames = static_cast<std::size_t>(
      audio::MillisecondsToFrames(kBufferMs, kDeviceFormat.sample_rate_hz));
  player_ = std::make_unique<audio::Player>(config);

  device_ = platform::CreateAudioDevice();
  const bool started = device_->Start(
      platform::AudioDeviceFormat{kDeviceFormat.sample_rate_hz, kDeviceFormat.channels},
      [](float* output, std::size_t frames, void* user_data) {
        static_cast<audio::Player*>(user_data)->Render(output, frames);
      },
      player_.get());

  if (!started) {
    device_.reset();
    player_.reset();
    return false;
  }

  const platform::AudioDeviceFormat opened = device_->format();
  description_ = device_->description() + ", " + std::to_string(opened.sample_rate_hz) +
                 " Hz, " + std::to_string(opened.channels) + " ch";

  audio::Player* player = player_.get();
  pump_ = std::make_unique<audio::DecodeThread>([player] { return player->Pump(); },
                                                std::chrono::milliseconds(5));
  return true;
}

void PlayerHost::Stop() {
  if (state_timer_) {
    state_timer_->Cancel();
    state_timer_ = nullptr;
  }
  // Device first: once it has stopped, the callback cannot run again, and only
  // then is it safe to let go of what the callback borrows.
  if (device_) {
    device_->Stop();
    device_.reset();
  }
  pump_.reset();
  player_.reset();
}

void PlayerHost::StartStateEvents(bridge::EventSink& sink) {
  if (!player_) {
    return;
  }
  bridge::EventSink* sink_pointer = &sink;
  state_timer_ = ShellTimer::Every(kStateSampleMs, [this, sink_pointer] {
    if (!player_) {
      return;
    }
    bridge::PlayerStateEvent payload;
    payload.player = State();
    bridge::Events(*sink_pointer).PlayerState(payload);
  });
}

bridge::PlayerState PlayerHost::State() const {
  bridge::PlayerState state;
  if (!player_) {
    state.state = "idle";
    state.trackIndex = -1;
    return state;
  }

  const audio::Player::Snapshot snapshot = player_->snapshot();
  state.state = std::string(core::ToString(snapshot.state));
  state.trackIndex = snapshot.track_index;
  state.queueSize = snapshot.queue_size;
  state.positionMs = snapshot.position_ms;
  state.durationMs = snapshot.duration_ms;
  state.volume = snapshot.volume;
  state.currentPath = snapshot.current_path;
  state.underruns = static_cast<std::int64_t>(snapshot.underruns);
  state.trackChanges = static_cast<std::int64_t>(snapshot.track_changes);
  state.lastError = snapshot.last_error;
  return state;
}

}  // namespace sonora::shell
