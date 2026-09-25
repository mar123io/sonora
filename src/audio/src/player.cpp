#include <sonora/audio/player.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <utility>

#include <sonora/audio/codecs.h>

namespace sonora::audio {
namespace {

using core::PlaybackState;

[[nodiscard]] float Clamp01(float value) noexcept {
  return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

// Pressing "previous" in the first few seconds of a track means the previous
// track; pressing it later means the start of this one. Every player does
// this, and it is the behaviour people expect rather than a preference.
constexpr std::int64_t kRestartThresholdMs = 3000;

// Not path::string(), which on Windows converts to the system's narrow code page
// and throws std::filesystem_error on anything it cannot represent -- so a track
// in D:\Müsik\ would have taken the snapshot down on a machine whose code page
// is not the right one. u8string always works, and UTF-8 is what everything
// above this target speaks anyway.
[[nodiscard]] std::string PathToUtf8(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return std::string(text.begin(), text.end());
}

}  // namespace

Player::Player(Config config) : config_(config) {
  if (!config_.device_format.IsValid()) {
    throw std::invalid_argument("Player: the device format is required");
  }
  if (!config_.open_track) {
    config_.open_track = [](const std::filesystem::path& path, AudioFormat target) {
      return OpenFileDecoder(path, target);
    };
  }
  const double ramp_frames = static_cast<double>(config_.volume_ramp_ms) *
                             config_.device_format.sample_rate_hz / 1000.0;
  gain_step_per_frame_ = ramp_frames > 1.0 ? static_cast<float>(1.0 / ramp_frames) : 1.0f;
}

Player::~Player() {
  // The device must already have been stopped: the destructor cannot make
  // Render() stop being called, it can only be the last thing that happens.
  for (auto& slot : render_slots_) {
    slot.store(nullptr, std::memory_order_release);
  }
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

void Player::Post(Command command) {
  const std::lock_guard<std::mutex> lock(control_mutex_);
  commands_.push_back(std::move(command));
}

void Player::Enqueue(std::filesystem::path path) {
  Post(Command{.type = CommandType::kEnqueue, .path = std::move(path)});
}

void Player::ClearQueue() {
  Post(Command{.type = CommandType::kClearQueue});
}

void Player::Play() {
  // Resuming is instant and structural starting is not: un-pausing only has to
  // change a bool the callback already reads, while starting a track has to
  // open a file. Both are posted so the state machine has one owner, but the
  // bool is also cleared here so a pause/resume never waits for the pump.
  paused_.store(false, std::memory_order_relaxed);
  Post(Command{.type = CommandType::kPlay});
}

void Player::Pause() {
  paused_.store(true, std::memory_order_relaxed);
}

void Player::Stop() {
  Post(Command{.type = CommandType::kStop});
}

void Player::Next() {
  Post(Command{.type = CommandType::kNext});
}

void Player::Previous() {
  Post(Command{.type = CommandType::kPrevious});
}

void Player::PlayTrack(int index) {
  Post(Command{.type = CommandType::kJumpTo, .track_index = index});
}

void Player::SeekMs(std::int64_t position_ms) {
  Post(Command{.type = CommandType::kSeek,
               .position_ms = std::max<std::int64_t>(0, position_ms)});
}

void Player::SetVolume(float linear) {
  target_gain_.store(Clamp01(linear), std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// The device thread
// ---------------------------------------------------------------------------

void Player::Render(float* output, std::size_t frames) noexcept {
  render_in_progress_.store(true, std::memory_order_release);

  const std::size_t channels = config_.device_format.SamplesPerFrame();
  const bool paused = paused_.load(std::memory_order_relaxed);
  std::size_t filled = 0;

  // A paused player keeps consuming until the fade-out has finished. Cutting
  // to zero on the frame the button was pressed is a click; fading and then
  // stopping means the twenty milliseconds that were consumed are the twenty
  // milliseconds that were heard, fading, which is what a fade-out is.
  const bool may_consume = !paused || current_gain_ > 0.0f;

  if (may_consume) {
    int slot = active_slot_.load(std::memory_order_acquire);
    while (filled < frames) {
      TrackStream* stream = render_slots_[slot].load(std::memory_order_acquire);
      if (stream == nullptr) {
        break;
      }

      filled += stream->Read(output + filled * channels, frames - filled);
      if (filled == frames) {
        break;
      }

      // A short read with the track still running is an underrun: the decode
      // thread is late. A short read with the track finished is the end of it,
      // and the moment this whole class exists for.
      if (!stream->finished()) {
        break;
      }

      const int other = 1 - slot;
      if (render_slots_[other].load(std::memory_order_acquire) == nullptr) {
        break;  // the queue is over; the rest of this buffer is silence
      }

      // The gapless join. It happens here, inside the callback, in the middle
      // of a buffer, without asking any other thread for anything -- because
      // anything that waits is the hole this is meant to remove.
      slot = other;
      active_slot_.store(slot, std::memory_order_release);
      active_track_index_.store(render_slot_track_[slot].load(std::memory_order_acquire),
                                std::memory_order_relaxed);
      track_changes_.fetch_add(1, std::memory_order_relaxed);
    }

    if (filled < frames) {
      const TrackStream* stream =
          render_slots_[active_slot_.load(std::memory_order_relaxed)].load(
              std::memory_order_acquire);
      if (stream != nullptr && !stream->finished()) {
        underruns_.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }

  if (filled < frames) {
    // Silence, not whatever the previous callback left in the buffer.
    std::memset(output + filled * channels, 0, (frames - filled) * channels * sizeof(float));
  }

  ApplyGain(output, frames);

  frames_rendered_.fetch_add(frames, std::memory_order_relaxed);
  render_epoch_.fetch_add(1, std::memory_order_release);
  render_in_progress_.store(false, std::memory_order_release);
}

void Player::ApplyGain(float* output, std::size_t frames) noexcept {
  const std::size_t channels = config_.device_format.SamplesPerFrame();
  const float target = paused_.load(std::memory_order_relaxed)
                           ? 0.0f
                           : target_gain_.load(std::memory_order_relaxed);

  if (current_gain_ == target) {
    if (current_gain_ == 1.0f) {
      return;  // the common case costs nothing
    }
    for (std::size_t i = 0; i < frames * channels; ++i) {
      output[i] *= current_gain_;
    }
    return;
  }

  float gain = current_gain_;
  for (std::size_t frame = 0; frame < frames; ++frame) {
    if (gain < target) {
      gain = std::min(target, gain + gain_step_per_frame_);
    } else {
      gain = std::max(target, gain - gain_step_per_frame_);
    }
    for (std::size_t channel = 0; channel < channels; ++channel) {
      output[frame * channels + channel] *= gain;
    }
  }
  current_gain_ = gain;
}

// ---------------------------------------------------------------------------
// The decode thread
// ---------------------------------------------------------------------------

void Player::WaitForRenderToLeave() noexcept {
  // Called after a slot has been cleared, so a callback starting from now sees
  // no stream. This only waits out a callback that was already inside one.
  //
  // It spins on the decode thread, never on the device thread, and it finishes
  // immediately when no device is running -- which is what lets the tests
  // drive Pump() and Render() by hand in any order.
  for (int attempt = 0; attempt < 200; ++attempt) {
    if (!render_in_progress_.load(std::memory_order_acquire)) {
      return;
    }
    std::this_thread::yield();
  }
}

bool Player::OpenInto(int slot_index, int track_index) {
  std::filesystem::path path;
  {
    const std::lock_guard<std::mutex> lock(control_mutex_);
    if (track_index < 0 || track_index >= static_cast<int>(queue_.size())) {
      return false;
    }
    path = queue_[static_cast<std::size_t>(track_index)];
  }

  TrackStream::Config stream_config;
  stream_config.ring_frames = config_.ring_frames;

  try {
    // Opened at the device's format, so the join between two tracks is a copy
    // and not a conversion.
    auto decoder = config_.open_track(path, config_.device_format);
    auto stream = std::make_unique<TrackStream>(std::move(decoder), stream_config);
    stream->Prime();

    slots_[slot_index].stream = std::move(stream);
    slots_[slot_index].track_index = track_index;
    render_slot_track_[slot_index].store(track_index, std::memory_order_release);
    // Published last: the device thread must never see a stream that has not
    // been primed.
    render_slots_[slot_index].store(slots_[slot_index].stream.get(), std::memory_order_release);
    return true;
  } catch (const DecoderError& error) {
    const std::lock_guard<std::mutex> lock(control_mutex_);
    last_error_ = error.what();
    return false;
  }
}

void Player::RetireSlot(int slot_index) {
  if (!slots_[slot_index].stream) {
    return;
  }
  render_slots_[slot_index].store(nullptr, std::memory_order_release);
  render_slot_track_[slot_index].store(-1, std::memory_order_release);
  WaitForRenderToLeave();

  // Destroyed on the next Pump rather than here: freeing memory is not a
  // real-time hazard on this thread, but keeping the two steps apart makes the
  // ordering -- unpublish, then let go -- impossible to get wrong later.
  retiring_.push_back(std::move(slots_[slot_index].stream));
  slots_[slot_index].track_index = -1;
  retire_epoch_ = render_epoch_.load(std::memory_order_acquire);
}

void Player::ApplyCommand(const Command& command) {
  switch (command.type) {
    case CommandType::kEnqueue: {
      {
        const std::lock_guard<std::mutex> lock(control_mutex_);
        queue_.push_back(command.path);
      }
      queue_version_.fetch_add(1, std::memory_order_relaxed);
      break;
    }

    case CommandType::kClearQueue: {
      RetireSlot(0);
      RetireSlot(1);
      {
        const std::lock_guard<std::mutex> lock(control_mutex_);
        queue_.clear();
      }
      queue_version_.fetch_add(1, std::memory_order_relaxed);
      active_track_index_.store(-1, std::memory_order_relaxed);
      next_track_to_open_ = -1;
      machine_.Reset();
      state_.store(PlaybackState::kIdle, std::memory_order_relaxed);
      break;
    }

    case CommandType::kPlay: {
      if (active_track_index_.load(std::memory_order_relaxed) >= 0) {
        // Already on a track: Play() has already cleared the pause flag, and
        // there is nothing structural left to do.
        machine_.TransitionTo(PlaybackState::kPlaying);
        state_.store(machine_.state(), std::memory_order_relaxed);
        break;
      }
      std::size_t queue_size = 0;
      {
        const std::lock_guard<std::mutex> lock(control_mutex_);
        queue_size = queue_.size();
      }
      if (queue_size == 0) {
        break;
      }
      machine_.TransitionTo(PlaybackState::kLoading);
      state_.store(machine_.state(), std::memory_order_relaxed);
      pump_active_slot_ = active_slot_.load(std::memory_order_relaxed);
      if (OpenInto(pump_active_slot_, 0)) {
        active_track_index_.store(0, std::memory_order_relaxed);
        next_track_to_open_ = 1;
        machine_.TransitionTo(PlaybackState::kPlaying);
      } else {
        machine_.TransitionTo(PlaybackState::kStopped);
      }
      state_.store(machine_.state(), std::memory_order_relaxed);
      break;
    }

    case CommandType::kStop: {
      RetireSlot(0);
      RetireSlot(1);
      active_track_index_.store(-1, std::memory_order_relaxed);
      next_track_to_open_ = -1;
      machine_.TransitionTo(PlaybackState::kStopped);
      state_.store(machine_.state(), std::memory_order_relaxed);
      break;
    }

    case CommandType::kNext:
    case CommandType::kPrevious:
    case CommandType::kJumpTo: {
      const int current = active_track_index_.load(std::memory_order_relaxed);
      // A jump says which track it wants, so it works from idle and from
      // stopped; next and previous are relative and mean nothing there.
      if (current < 0 && command.type != CommandType::kJumpTo) {
        break;
      }
      int wanted = command.track_index;
      if (command.type == CommandType::kJumpTo) {
        // Already decided.
      } else if (command.type == CommandType::kNext) {
        wanted = current + 1;
      } else if (PositionMs() > kRestartThresholdMs) {
        wanted = current;  // restart this one
      } else {
        wanted = current - 1;
      }

      std::size_t queue_size = 0;
      {
        const std::lock_guard<std::mutex> lock(control_mutex_);
        queue_size = queue_.size();
      }
      if (wanted < 0 || wanted >= static_cast<int>(queue_size)) {
        break;
      }

      // From idle or stopped, a jump is a load: every state may enter kLoading,
      // and nothing may go straight from kIdle to kPlaying.
      if (current < 0) {
        machine_.TransitionTo(PlaybackState::kLoading);
        state_.store(machine_.state(), std::memory_order_relaxed);
      }

      // A skip is not a gapless join: both slots go, and the wanted track is
      // opened fresh. Reusing a preloaded next track would only help when the
      // user pressed exactly "next", and the special case is not worth the
      // second code path through the same machinery.
      RetireSlot(0);
      RetireSlot(1);
      pump_active_slot_ = active_slot_.load(std::memory_order_relaxed);
      if (OpenInto(pump_active_slot_, wanted)) {
        active_track_index_.store(wanted, std::memory_order_relaxed);
        next_track_to_open_ = wanted + 1;
        machine_.TransitionTo(PlaybackState::kPlaying);
        state_.store(machine_.state(), std::memory_order_relaxed);
      }
      break;
    }

    case CommandType::kSeek: {
      const int slot = active_slot_.load(std::memory_order_relaxed);
      if (!slots_[slot].stream) {
        break;
      }
      const std::uint64_t frame =
          MillisecondsToFrames(command.position_ms, config_.device_format.sample_rate_hz);

      // Unpublish, wait out any callback already inside, then seek. The device
      // thread never waits for this; it simply finds no stream for as long as
      // the seek takes and plays silence, which is what a seek sounds like.
      render_slots_[slot].store(nullptr, std::memory_order_release);
      WaitForRenderToLeave();

      if (slots_[slot].stream->SeekFrame(frame)) {
        slots_[slot].stream->Prime();
      }
      render_slots_[slot].store(slots_[slot].stream.get(), std::memory_order_release);
      break;
    }
  }
}

bool Player::Pump() {
  bool did_work = false;

  // Retired streams are destroyed one Pump after they were unpublished, and
  // only once the device thread has been through a callback since.
  if (!retiring_.empty() && render_epoch_.load(std::memory_order_acquire) != retire_epoch_) {
    retiring_.clear();
    did_work = true;
  }

  for (;;) {
    Command command;
    {
      const std::lock_guard<std::mutex> lock(control_mutex_);
      if (commands_.empty()) {
        break;
      }
      command = std::move(commands_.front());
      commands_.pop_front();
    }
    ApplyCommand(command);
    did_work = true;
  }

  // The device thread may have moved to the other slot since the last pump.
  // That is how a track change is discovered here: nothing tells us, the flag
  // simply reads differently.
  const int render_slot = active_slot_.load(std::memory_order_acquire);
  if (render_slot != pump_active_slot_) {
    RetireSlot(pump_active_slot_);
    pump_active_slot_ = render_slot;
    next_track_to_open_ = slots_[render_slot].track_index + 1;
    did_work = true;
  }

  TrackStream* active = slots_[pump_active_slot_].stream.get();
  if (active == nullptr) {
    return did_work;
  }

  if (active->DecodeStep() > 0) {
    did_work = true;
  }

  // Open the next track while there is still audio in hand for the current
  // one. "Still in hand" is measured from what the decoder has produced, not
  // from what has been heard: the buffer in front of the device is exactly the
  // time available to do this in.
  const int other = 1 - pump_active_slot_;
  if (slots_[other].stream == nullptr && next_track_to_open_ >= 0) {
    const std::uint64_t total = active->total_frames();
    const std::uint64_t decoded = active->frames_decoded();
    const std::int64_t remaining_ms =
        total > decoded
            ? FramesToMilliseconds(total - decoded, config_.device_format.sample_rate_hz)
            : 0;

    // Two triggers, and the second is the one that matters: a track whose
    // length is unknown -- a VBR MP3, a stream -- never crosses the first.
    if (remaining_ms <= config_.preload_ahead_ms || active->exhausted()) {
      if (OpenInto(other, next_track_to_open_)) {
        did_work = true;
      } else {
        next_track_to_open_ = -1;  // nothing more to preload
      }
    }
  }

  if (slots_[other].stream != nullptr) {
    if (slots_[other].stream->DecodeStep() > 0) {
      did_work = true;
    }
  }

  // The queue ran out and the last track has been heard to the end.
  if (active->finished() && slots_[other].stream == nullptr &&
      state_.load(std::memory_order_relaxed) == PlaybackState::kPlaying) {
    machine_.TransitionTo(PlaybackState::kStopped);
    state_.store(machine_.state(), std::memory_order_relaxed);
    did_work = true;
  }

  return did_work;
}

// ---------------------------------------------------------------------------

std::int64_t Player::PositionMs() const noexcept {
  const int slot = active_slot_.load(std::memory_order_relaxed);
  const TrackStream* stream = render_slots_[slot].load(std::memory_order_acquire);
  if (stream == nullptr) {
    return 0;
  }
  return FramesToMilliseconds(stream->frames_read(), config_.device_format.sample_rate_hz);
}

Player::Snapshot Player::snapshot() const {
  Snapshot snapshot;
  snapshot.state = paused_.load(std::memory_order_relaxed) &&
                           state_.load(std::memory_order_relaxed) == PlaybackState::kPlaying
                       ? PlaybackState::kPaused
                       : state_.load(std::memory_order_relaxed);
  snapshot.track_index = active_track_index_.load(std::memory_order_relaxed);
  snapshot.volume = target_gain_.load(std::memory_order_relaxed);
  snapshot.underruns = underruns_.load(std::memory_order_relaxed);
  snapshot.frames_rendered = frames_rendered_.load(std::memory_order_relaxed);
  snapshot.track_changes = track_changes_.load(std::memory_order_relaxed);
  snapshot.position_ms = PositionMs();

  const int slot = active_slot_.load(std::memory_order_relaxed);
  if (const TrackStream* stream = render_slots_[slot].load(std::memory_order_acquire)) {
    snapshot.duration_ms =
        FramesToMilliseconds(stream->total_frames(), config_.device_format.sample_rate_hz);
  }

  const std::lock_guard<std::mutex> lock(control_mutex_);
  snapshot.queue_size = static_cast<int>(queue_.size());
  snapshot.queue_version = queue_version_.load(std::memory_order_relaxed);
  snapshot.last_error = last_error_;
  if (snapshot.track_index >= 0 && snapshot.track_index < snapshot.queue_size) {
    snapshot.current_path = PathToUtf8(queue_[static_cast<std::size_t>(snapshot.track_index)]);
  }
  return snapshot;
}

std::vector<std::string> Player::queue_paths() const {
  const std::lock_guard<std::mutex> lock(control_mutex_);
  std::vector<std::string> paths;
  paths.reserve(queue_.size());
  for (const std::filesystem::path& path : queue_) {
    paths.push_back(PathToUtf8(path));
  }
  return paths;
}

}  // namespace sonora::audio
