#pragma once

#include <memory>
#include <string>

#include <bridge_generated.h>
#include <sonora/audio/format.h>

#include "cef/timer.h"

namespace sonora::audio {
class DecodeThread;
class Player;
}  // namespace sonora::audio

namespace sonora::platform {
class AudioDevice;
}  // namespace sonora::platform

namespace sonora::shell {

// Owns the player, the device it feeds and the thread that decodes into it,
// and turns the player's state into events on the bridge.
//
// It exists so that runtime.cpp does not: three objects with a strict creation
// and destruction order -- device last to start, first to stop, because the
// callback borrows the player -- are better kept together than spread across
// a file that is mostly about CEF.
class PlayerHost {
 public:
  PlayerHost();
  ~PlayerHost();

  PlayerHost(const PlayerHost&) = delete;
  PlayerHost& operator=(const PlayerHost&) = delete;

  // Opens the default output device and starts the decode thread. False when
  // there is no device, which is not a fatal condition: the shell disables the
  // player capability and the page hides the transport, which is the degraded
  // path it already knows how to take.
  bool Start();

  // Stops the device, then the decode thread, then lets go of the player, in
  // that order. Idempotent, and called by the destructor.
  void Stop();

  // Begins emitting player.state. Called once the event channel exists.
  void StartStateEvents(bridge::EventSink& sink);

  // Null until Start() has succeeded.
  [[nodiscard]] audio::Player* player() const { return player_.get(); }

  // What the device actually opened at, for the startup line.
  [[nodiscard]] std::string description() const { return description_; }

  // Fills a generated PlayerState from the player's snapshot. One place, so
  // the answer to player.getState and the payload of player.state cannot drift
  // apart.
  [[nodiscard]] bridge::PlayerState State() const;

 private:
  std::unique_ptr<audio::Player> player_;
  std::unique_ptr<platform::AudioDevice> device_;
  std::unique_ptr<audio::DecodeThread> pump_;
  CefRefPtr<ShellTimer> state_timer_;
  std::string description_;
};

}  // namespace sonora::shell
