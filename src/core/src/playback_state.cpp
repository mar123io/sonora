#include <sonora/core/playback_state.h>

namespace sonora::core {

std::string_view ToString(PlaybackState state) noexcept {
  switch (state) {
    case PlaybackState::kIdle:
      return "idle";
    case PlaybackState::kLoading:
      return "loading";
    case PlaybackState::kPlaying:
      return "playing";
    case PlaybackState::kPaused:
      return "paused";
    case PlaybackState::kStopped:
      return "stopped";
  }
  return "invalid";
}

bool IsTransitionAllowed(PlaybackState from, PlaybackState to) noexcept {
  if (from == to) {
    return false;
  }
  switch (from) {
    case PlaybackState::kIdle:
      // Only a load can leave the empty state.
      return to == PlaybackState::kLoading;
    case PlaybackState::kLoading:
      // Success, failure, or the user cancelling before the first sample.
      return to == PlaybackState::kPlaying || to == PlaybackState::kStopped ||
             to == PlaybackState::kIdle;
    case PlaybackState::kPlaying:
      // kLoading is the gapless/skip path: next source opens while this one drains.
      return to == PlaybackState::kPaused || to == PlaybackState::kStopped ||
             to == PlaybackState::kLoading;
    case PlaybackState::kPaused:
      return to == PlaybackState::kPlaying || to == PlaybackState::kStopped ||
             to == PlaybackState::kLoading;
    case PlaybackState::kStopped:
      return to == PlaybackState::kLoading || to == PlaybackState::kIdle;
  }
  return false;
}

bool PlaybackStateMachine::TransitionTo(PlaybackState next) noexcept {
  if (!IsTransitionAllowed(state_, next)) {
    return false;
  }
  state_ = next;
  return true;
}

}  // namespace sonora::core
