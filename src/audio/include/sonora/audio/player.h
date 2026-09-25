#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <sonora/audio/decoder.h>
#include <sonora/audio/format.h>
#include <sonora/audio/track_stream.h>
#include <sonora/core/playback_state.h>

namespace sonora::audio {

// A queue of tracks, played through one device that is opened once and never
// reopened.
//
// Three threads meet here, and which one may do what is the whole design:
//
//   commands   the UI thread. Queues work and returns; never blocks, never
//              opens a file, never waits for audio.
//   Pump()     the decode thread. Does every slow thing: opening files,
//              decoding, seeking, destroying finished tracks.
//   Render()   the device thread. Reads what is there, and nothing else.
//              ADR 0006 applies to it and to everything it calls.
//
// The gapless join is the reason Render() is allowed to change tracks at all.
// A track that ends halfway through a callback has to be followed by the next
// one in the same buffer, in the same call -- anything that waits for another
// thread, or for the next callback, is a hole you can hear between two tracks
// of a live album. So Render() switches slots itself, and the decode thread's
// job is to have the next track already decoded before that moment arrives.
class Player {
 public:
  struct Config {
    // Fixed for the player's life. Every track is opened at this format, so a
    // track change needs no conversion and no reopened device.
    AudioFormat device_format;
    std::size_t ring_frames = 24000;
    int volume_ramp_ms = 20;
    // How much of the current track has to be left before the next one is
    // opened. Generous on purpose: opening and priming a FLAC is milliseconds
    // on a warm cache and can be much worse on a cold one.
    std::int64_t preload_ahead_ms = 5000;

    // How a queue entry becomes a decoder. Empty means OpenFileDecoder, which
    // is what the shell wants.
    //
    // It is injectable because of the tests: handing back a generated ramp
    // instead of a file is what lets the gapless join be checked sample by
    // sample, with no filesystem, no sound card and no timing. It also means
    // Player itself knows nothing about files, which turned out to be the
    // better shape anyway.
    std::function<DecoderPtr(const std::filesystem::path&, AudioFormat)> open_track;
  };

  struct Snapshot {
    core::PlaybackState state = core::PlaybackState::kIdle;
    int track_index = -1;
    int queue_size = 0;
    std::int64_t position_ms = 0;
    std::int64_t duration_ms = 0;
    float volume = 1.0f;
    std::uint64_t underruns = 0;
    std::uint64_t frames_rendered = 0;
    // Gapless joins performed inside a callback. The number week 6 exists to
    // make non-zero.
    std::uint64_t track_changes = 0;
    std::string current_path;
    std::string last_error;
  };

  explicit Player(Config config);
  ~Player();

  Player(const Player&) = delete;
  Player& operator=(const Player&) = delete;

  // ---- commands ----------------------------------------------------------
  // Safe from one thread at a time, and none of them blocks on audio. Volume
  // and pause take effect in the next callback; everything else takes effect
  // on the next Pump(), because everything else needs a file opened.
  void Enqueue(std::filesystem::path path);
  void ClearQueue();
  void Play();
  void Pause();
  void Stop();
  void Next();
  void Previous();
  void SeekMs(std::int64_t position_ms);
  void SetVolume(float linear);

  // ---- device thread -----------------------------------------------------
  // REAL TIME. Always writes exactly `frames` frames.
  void Render(float* output, std::size_t frames) noexcept;

  // ---- decode thread -----------------------------------------------------
  // Everything slow. Returns true when it did work, so the caller knows
  // whether to come straight back or to sleep.
  bool Pump();

  [[nodiscard]] Snapshot snapshot() const;
  [[nodiscard]] AudioFormat format() const noexcept { return config_.device_format; }

 private:
  enum class CommandType { kPlay, kStop, kNext, kPrevious, kSeek, kEnqueue, kClearQueue };

  struct Command {
    CommandType type = CommandType::kPlay;
    std::int64_t position_ms = 0;
    std::filesystem::path path;
  };

  // Storage owned by the decode thread. render_slots_ is what the device
  // thread is allowed to look at, and it is only ever published once a stream
  // is primed and only ever cleared before the stream is touched again.
  struct Slot {
    std::unique_ptr<TrackStream> stream;
    int track_index = -1;
  };

  void Post(Command command);
  void ApplyCommand(const Command& command);
  bool OpenInto(int slot_index, int track_index);
  void RetireSlot(int slot_index);
  void WaitForRenderToLeave() noexcept;
  void ApplyGain(float* output, std::size_t frames) noexcept;
  [[nodiscard]] std::int64_t PositionMs() const noexcept;

  Config config_;

  mutable std::mutex control_mutex_;  // queue_, commands_, paths, last_error_
  std::deque<Command> commands_;
  std::vector<std::filesystem::path> queue_;
  std::string last_error_;

  Slot slots_[2];
  int pump_active_slot_ = 0;
  int next_track_to_open_ = -1;

  std::vector<std::unique_ptr<TrackStream>> retiring_;
  std::uint64_t retire_epoch_ = 0;

  core::PlaybackStateMachine machine_;

  // Read by the device thread.
  std::atomic<TrackStream*> render_slots_[2]{nullptr, nullptr};
  // Published with the stream, so the device thread can say which track it
  // moved to without asking the decode thread anything.
  std::atomic<int> render_slot_track_[2]{-1, -1};
  std::atomic<int> active_slot_{0};
  std::atomic<bool> paused_{false};
  std::atomic<bool> render_in_progress_{false};
  std::atomic<std::uint64_t> render_epoch_{0};

  std::atomic<float> target_gain_{1.0f};
  float current_gain_ = 1.0f;
  float gain_step_per_frame_ = 1.0f;

  std::atomic<int> active_track_index_{-1};
  std::atomic<core::PlaybackState> state_{core::PlaybackState::kIdle};
  std::atomic<std::uint64_t> frames_rendered_{0};
  std::atomic<std::uint64_t> underruns_{0};
  std::atomic<std::uint64_t> track_changes_{0};
};

}  // namespace sonora::audio
