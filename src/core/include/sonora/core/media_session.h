#pragma once

#include <cstdint>
#include <string>

#include <sonora/core/playback_state.h>

namespace sonora::core {

// What the system panel currently shows, as far as this side is concerned.
//
// The cover is an opaque key rather than the picture: the policy below only
// ever has to answer "is this a different cover", and carrying a megabyte of
// JPEG through a comparison to answer it would be a strange way to spend
// memory. The shell passes the content hash it already has.
struct NowPlaying {
  std::string title;
  std::string artist;
  std::string album;
  std::string art_key;
  int track_number = 0;
  std::int64_t duration_ms = 0;

  [[nodiscard]] bool operator==(const NowPlaying& other) const;
  [[nodiscard]] bool operator!=(const NowPlaying& other) const { return !(*this == other); }
};

// Which of the three updates are worth making.
struct MediaUpdate {
  bool metadata = false;
  bool state = false;
  bool timeline = false;

  [[nodiscard]] bool any() const { return metadata || state || timeline; }
};

// Turns the stream of player states into the few updates the operating system
// actually needs.
//
// It exists because the two rates do not match, and because of what happens
// when you ignore that. The player's state is sampled ten times a second; the
// panel needs the title when the track changes, the state when it changes, and
// the position about once a second. Pushing everything on every sample means
// handing the system a picture to decode ten times a second for a cover that
// has not changed -- which is measurable, and which is exactly the mistake the
// event coalescer exists to prevent on the other side of the bridge.
//
// It is also the one part of the media integration that can be tested. There is
// no SystemMediaTransportControls on a Linux CI runner and there never will be,
// so the rules live here, in a class that knows nothing about any operating
// system and is checked on all three.
class MediaSessionPolicy {
 public:
  struct Config {
    // The panel's clock only shows seconds.
    std::int64_t timeline_interval_ms = 1000;

    // A position that moved further than playback could have moved is a seek,
    // and a seek has to be shown immediately: the whole point of the panel's
    // progress bar is that it agrees with the audio.
    std::int64_t seek_threshold_ms = 1500;
  };

  // Two constructors rather than one with a defaulted argument: a default
  // argument that brace-initialises a nested type is evaluated before the
  // enclosing class is complete, and GCC says so.
  MediaSessionPolicy() = default;
  explicit MediaSessionPolicy(Config config) : config_(config) {}

  // `now_ms` is a monotonic clock supplied by the caller rather than read here,
  // so the tests can advance time instead of sleeping through it.
  [[nodiscard]] MediaUpdate Observe(const NowPlaying& now_playing,
                                    PlaybackState state,
                                    std::int64_t position_ms,
                                    std::int64_t now_ms);

  // After this the next Observe reports everything as changed. For when the
  // session itself was closed and reopened and the system has forgotten.
  void Reset();

  [[nodiscard]] const NowPlaying& now_playing() const { return now_playing_; }
  [[nodiscard]] PlaybackState state() const { return state_; }

 private:
  Config config_;
  bool started_ = false;
  NowPlaying now_playing_;
  PlaybackState state_ = PlaybackState::kIdle;
  std::int64_t position_ms_ = 0;
  std::int64_t timeline_pushed_at_ms_ = 0;
};

// The panel's vocabulary is smaller than the player's: loading is playing as
// far as anyone watching is concerned, and idle is not a session at all.
[[nodiscard]] bool IsSessionOpen(PlaybackState state);

}  // namespace sonora::core
