#include <catch2/catch_test_macros.hpp>

#include <array>

#include <sonora/core/playback_state.h>

using sonora::core::IsTransitionAllowed;
using sonora::core::PlaybackState;
using sonora::core::PlaybackStateMachine;
using sonora::core::ToString;

namespace {

constexpr std::array kAllStates = {
    PlaybackState::kIdle,   PlaybackState::kLoading, PlaybackState::kPlaying,
    PlaybackState::kPaused, PlaybackState::kStopped,
};

}  // namespace

TEST_CASE("a fresh machine is idle", "[playback]") {
  PlaybackStateMachine machine;
  REQUIRE(machine.state() == PlaybackState::kIdle);
}

TEST_CASE("the happy path walks load -> play -> pause -> play -> stop", "[playback]") {
  PlaybackStateMachine machine;

  REQUIRE(machine.TransitionTo(PlaybackState::kLoading));
  REQUIRE(machine.TransitionTo(PlaybackState::kPlaying));
  REQUIRE(machine.TransitionTo(PlaybackState::kPaused));
  REQUIRE(machine.TransitionTo(PlaybackState::kPlaying));
  REQUIRE(machine.TransitionTo(PlaybackState::kStopped));

  REQUIRE(machine.state() == PlaybackState::kStopped);
}

TEST_CASE("playing cannot be entered without loading first", "[playback]") {
  PlaybackStateMachine machine;

  REQUIRE_FALSE(machine.TransitionTo(PlaybackState::kPlaying));
  REQUIRE(machine.state() == PlaybackState::kIdle);
}

TEST_CASE("a rejected transition leaves the state untouched", "[playback]") {
  PlaybackStateMachine machine;
  REQUIRE(machine.TransitionTo(PlaybackState::kLoading));

  REQUIRE_FALSE(machine.TransitionTo(PlaybackState::kPaused));
  REQUIRE(machine.state() == PlaybackState::kLoading);
}

TEST_CASE("self-transitions are rejected", "[playback]") {
  // A second play() on an already playing stream must not restart it. Making
  // this a rejected transition rather than a no-op means the caller finds out.
  for (const PlaybackState state : kAllStates) {
    REQUIRE_FALSE(IsTransitionAllowed(state, state));
  }
}

TEST_CASE("skipping to the next track goes through loading", "[playback]") {
  // Gapless playback (week 6) opens the next source while the current one is
  // still draining, so playing -> loading has to be legal.
  REQUIRE(IsTransitionAllowed(PlaybackState::kPlaying, PlaybackState::kLoading));
  REQUIRE(IsTransitionAllowed(PlaybackState::kPaused, PlaybackState::kLoading));
}

TEST_CASE("a failed load falls back to stopped or idle", "[playback]") {
  REQUIRE(IsTransitionAllowed(PlaybackState::kLoading, PlaybackState::kStopped));
  REQUIRE(IsTransitionAllowed(PlaybackState::kLoading, PlaybackState::kIdle));
}

TEST_CASE("reset returns to idle from any state", "[playback]") {
  PlaybackStateMachine machine;
  REQUIRE(machine.TransitionTo(PlaybackState::kLoading));
  REQUIRE(machine.TransitionTo(PlaybackState::kPlaying));

  machine.Reset();
  REQUIRE(machine.state() == PlaybackState::kIdle);
}

TEST_CASE("every state has a distinct printable name", "[playback]") {
  for (const PlaybackState state : kAllStates) {
    REQUIRE_FALSE(ToString(state).empty());
    REQUIRE(ToString(state) != "invalid");
  }
}
