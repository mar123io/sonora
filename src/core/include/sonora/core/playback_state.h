#pragma once

#include <string_view>

namespace sonora::core {

// The playback lifecycle, kept deliberately small. Week 6 grows this into the
// full engine state machine; the point of having it now is that the transition
// table is the one piece of behaviour worth pinning down before any audio code
// exists, and it gives the test suite something real to assert on from day one.
enum class PlaybackState {
  kIdle,     // nothing loaded
  kLoading,  // decoder opening a source, no audio yet
  kPlaying,  // device callback is consuming samples
  kPaused,   // device open, callback outputs silence
  kStopped,  // playback ended or was aborted, source still known
};

[[nodiscard]] std::string_view ToString(PlaybackState state) noexcept;

// Transitions are explicit rather than "anything goes": an illegal transition
// is a bug in the caller, not a state to recover from. Self-transitions are
// rejected so that a redundant play() cannot silently restart a stream.
[[nodiscard]] bool IsTransitionAllowed(PlaybackState from, PlaybackState to) noexcept;

class PlaybackStateMachine {
 public:
  PlaybackStateMachine() = default;

  [[nodiscard]] PlaybackState state() const noexcept { return state_; }

  // Returns false and leaves the state untouched when the transition is not
  // allowed. Callers decide whether that is worth logging; the machine itself
  // stays silent so it remains usable from a real-time context later on.
  bool TransitionTo(PlaybackState next) noexcept;

  void Reset() noexcept { state_ = PlaybackState::kIdle; }

 private:
  PlaybackState state_ = PlaybackState::kIdle;
};

}  // namespace sonora::core
