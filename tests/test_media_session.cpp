#include <catch2/catch_test_macros.hpp>

#include <string>

#include <sonora/core/media_session.h>

using namespace sonora::core;

namespace {

[[nodiscard]] NowPlaying Track(const std::string& title, const std::string& art = "art1") {
  NowPlaying now;
  now.title = title;
  now.artist = "Artist";
  now.album = "Album";
  now.art_key = art;
  now.duration_ms = 200000;
  return now;
}

// Advances the policy and discards what it says. Observe is [[nodiscard]] for a
// good reason -- ignoring its answer in the shell means not telling the system
// anything -- and these lines are setup, not the assertion.
void Advance(MediaSessionPolicy& policy,
             const NowPlaying& now_playing,
             PlaybackState state,
             std::int64_t position_ms,
             std::int64_t now_ms) {
  (void)policy.Observe(now_playing, state, position_ms, now_ms);
}

}  // namespace

TEST_CASE("the first observation sends everything", "[media]") {
  // Not because anything changed -- nothing has -- but because the system knows
  // nothing yet, and a policy that only reports differences would start by
  // reporting none.
  MediaSessionPolicy policy;
  const MediaUpdate first = policy.Observe(Track("One"), PlaybackState::kPlaying, 0, 1000);

  REQUIRE(first.metadata);
  REQUIRE(first.state);
  REQUIRE(first.timeline);
}

TEST_CASE("a steady state at ten samples a second is not ten updates", "[media]") {
  // The reason this class exists. The player is sampled every 100 ms; the panel
  // needs the position about once a second and the cover not at all.
  MediaSessionPolicy policy;
  Advance(policy, Track("One"), PlaybackState::kPlaying, 0, 0);

  int metadata = 0;
  int timeline = 0;
  for (int tick = 1; tick <= 30; ++tick) {
    const std::int64_t now = tick * 100;
    const MediaUpdate update = policy.Observe(Track("One"), PlaybackState::kPlaying, now, now);
    metadata += update.metadata ? 1 : 0;
    timeline += update.timeline ? 1 : 0;
  }

  REQUIRE(metadata == 0);
  // Three seconds of playback, three timeline pushes.
  REQUIRE(timeline == 3);
}

TEST_CASE("a track change sends the metadata and the timeline with it", "[media]") {
  // The timeline goes too, and not as a courtesy: without it the panel shows the
  // new title over the old progress, claiming the song that just started is
  // already half over.
  MediaSessionPolicy policy;
  Advance(policy, Track("One"), PlaybackState::kPlaying, 120000, 0);

  const MediaUpdate update = policy.Observe(Track("Two"), PlaybackState::kPlaying, 0, 100);

  REQUIRE(update.metadata);
  REQUIRE(update.timeline);
  REQUIRE_FALSE(update.state);
}

TEST_CASE("only the cover changing is still a metadata change", "[media]") {
  MediaSessionPolicy policy;
  Advance(policy, Track("One", "art1"), PlaybackState::kPlaying, 0, 0);

  REQUIRE(policy.Observe(Track("One", "art2"), PlaybackState::kPlaying, 100, 100).metadata);
  REQUIRE_FALSE(
      policy.Observe(Track("One", "art2"), PlaybackState::kPlaying, 200, 200).metadata);
}

TEST_CASE("pausing sends the state and the position", "[media]") {
  // A paused panel freezes its clock wherever the last push left it, so the
  // transition has to carry the position or the two disagree until playback
  // resumes.
  MediaSessionPolicy policy;
  Advance(policy, Track("One"), PlaybackState::kPlaying, 0, 0);
  Advance(policy, Track("One"), PlaybackState::kPlaying, 100, 100);

  const MediaUpdate update = policy.Observe(Track("One"), PlaybackState::kPaused, 200, 200);
  REQUIRE(update.state);
  REQUIRE(update.timeline);
  REQUIRE_FALSE(update.metadata);
}

TEST_CASE("a seek is shown at once, not at the next second", "[media]") {
  // The position moved further than playing could have moved it. Waiting for
  // the next scheduled push would leave the panel's progress bar disagreeing
  // with the audio for up to a second, which is precisely the thing somebody
  // dragging a progress bar is watching.
  MediaSessionPolicy policy;
  Advance(policy, Track("One"), PlaybackState::kPlaying, 0, 0);
  Advance(policy, Track("One"), PlaybackState::kPlaying, 100, 100);

  const MediaUpdate jumped = policy.Observe(Track("One"), PlaybackState::kPlaying, 90000, 200);
  REQUIRE(jumped.timeline);
  REQUIRE_FALSE(jumped.metadata);
  REQUIRE_FALSE(jumped.state);

  // And backwards, which is the same thing and easy to get wrong with an
  // unsigned subtraction.
  Advance(policy, Track("One"), PlaybackState::kPlaying, 90100, 300);
  REQUIRE(policy.Observe(Track("One"), PlaybackState::kPlaying, 1000, 400).timeline);
}

TEST_CASE("playing forward is not a seek", "[media]") {
  // The other half of the same rule: ordinary playback moves the position by
  // exactly the time that passed, and must not be mistaken for a jump.
  MediaSessionPolicy policy;
  Advance(policy, Track("One"), PlaybackState::kPlaying, 0, 0);

  int timeline = 0;
  for (int tick = 1; tick <= 9; ++tick) {
    const std::int64_t now = tick * 100;
    timeline +=
        policy.Observe(Track("One"), PlaybackState::kPlaying, now, now).timeline ? 1 : 0;
  }
  REQUIRE(timeline == 0);  // under one second, nothing scheduled, no jump
}

TEST_CASE("a paused track whose position does not move sends nothing", "[media]") {
  // Somebody who paused and walked away should not cost a system call a second
  // for the rest of the afternoon.
  MediaSessionPolicy policy;
  Advance(policy, Track("One"), PlaybackState::kPaused, 5000, 0);

  int updates = 0;
  for (int tick = 1; tick <= 50; ++tick) {
    const std::int64_t now = tick * 100;
    updates += policy.Observe(Track("One"), PlaybackState::kPaused, 5000, now).any() ? 1 : 0;
  }
  REQUIRE(updates == 0);
}

TEST_CASE("reset makes the next observation a first one", "[media]") {
  MediaSessionPolicy policy;
  Advance(policy, Track("One"), PlaybackState::kPlaying, 0, 0);
  REQUIRE_FALSE(policy.Observe(Track("One"), PlaybackState::kPlaying, 50, 50).metadata);

  policy.Reset();
  const MediaUpdate again = policy.Observe(Track("One"), PlaybackState::kPlaying, 50, 60);
  REQUIRE(again.metadata);
  REQUIRE(again.state);
  REQUIRE(again.timeline);
}

TEST_CASE("idle is not a session", "[media]") {
  REQUIRE_FALSE(IsSessionOpen(PlaybackState::kIdle));
  REQUIRE(IsSessionOpen(PlaybackState::kPlaying));
  REQUIRE(IsSessionOpen(PlaybackState::kPaused));
  // Stopped keeps the panel, with its buttons: the queue is still there and
  // pressing play on the keyboard should start it.
  REQUIRE(IsSessionOpen(PlaybackState::kStopped));
}
