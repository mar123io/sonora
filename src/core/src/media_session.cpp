#include <sonora/core/media_session.h>

#include <cstdlib>

namespace sonora::core {

bool NowPlaying::operator==(const NowPlaying& other) const {
  return title == other.title && artist == other.artist && album == other.album &&
         art_key == other.art_key && track_number == other.track_number &&
         duration_ms == other.duration_ms;
}

bool IsSessionOpen(PlaybackState state) {
  return state != PlaybackState::kIdle;
}

MediaUpdate MediaSessionPolicy::Observe(const NowPlaying& now_playing,
                                        PlaybackState state,
                                        std::int64_t position_ms,
                                        std::int64_t now_ms) {
  MediaUpdate update;

  // The first observation is all three, whatever it says. The system knows
  // nothing yet, and "nothing changed since the default-constructed value" is
  // the one case where nothing changed and everything still has to be sent.
  if (!started_) {
    started_ = true;
    now_playing_ = now_playing;
    state_ = state;
    position_ms_ = position_ms;
    timeline_pushed_at_ms_ = now_ms;
    return MediaUpdate{true, true, true};
  }

  if (now_playing != now_playing_) {
    now_playing_ = now_playing;
    update.metadata = true;
    // A new track with the old track's progress bar is worse than no progress
    // bar: for a moment the panel claims the new song is four minutes in.
    update.timeline = true;
  }

  if (state != state_) {
    state_ = state;
    update.state = true;
    // Pausing freezes the panel's clock, and it freezes it wherever the last
    // push left it. Sending the position with the transition is what keeps the
    // two from disagreeing for as long as the pause lasts.
    update.timeline = true;
  }

  // Did the position move further than playing could have moved it?
  const std::int64_t elapsed = now_ms - timeline_pushed_at_ms_;
  const std::int64_t moved = position_ms - position_ms_;
  const std::int64_t unexplained = moved - (state_ == PlaybackState::kPlaying ? elapsed : 0);
  if (std::llabs(static_cast<long long>(unexplained)) > config_.seek_threshold_ms) {
    update.timeline = true;
  }

  // The scheduled push is for a position that is moving. A paused track's
  // position is not, and the transition into pause already sent it -- so
  // somebody who paused and walked away costs nothing at all, rather than a
  // system call a second for the rest of the afternoon.
  if (!update.timeline && state_ == PlaybackState::kPlaying &&
      elapsed >= config_.timeline_interval_ms) {
    update.timeline = true;
  }

  position_ms_ = position_ms;
  if (update.timeline) {
    timeline_pushed_at_ms_ = now_ms;
  }
  return update;
}

void MediaSessionPolicy::Reset() {
  started_ = false;
  now_playing_ = NowPlaying{};
  state_ = PlaybackState::kIdle;
  position_ms_ = 0;
  timeline_pushed_at_ms_ = 0;
}

}  // namespace sonora::core
