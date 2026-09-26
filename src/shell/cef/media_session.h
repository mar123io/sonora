#pragma once

#include <memory>
#include <string>

#include <sonora/core/media_session.h>
#include <sonora/platform/media_integration.h>

#include "cef/timer.h"

namespace sonora::shell {

class LibraryHost;
class PlayerHost;

// Keeps the operating system's media panel in step with the player, and turns
// what the panel sends back into player commands.
//
// Three things meet here and none of them belongs to the others: the player,
// which knows what is playing and nothing about Windows; the library, which
// knows what the track is called and what its cover looks like; and the
// platform integration, which knows how to say that to the system. The policy
// for *when* to say it lives in sonora::core, where it can be tested on a
// machine that has no media panel at all.
//
// Threading: everything here belongs to the CEF UI thread, except the one
// method that does not -- the system's requests arrive on its own thread and
// are posted back here before anything is touched.
class ShellMediaSession {
 public:
  ShellMediaSession(PlayerHost& player, LibraryHost& library);
  ~ShellMediaSession();

  ShellMediaSession(const ShellMediaSession&) = delete;
  ShellMediaSession& operator=(const ShellMediaSession&) = delete;

  // `native_window` is the application window, not the browser view: the system
  // ties the session to a window and shows that window's application. False
  // when this system offers no media integration, which is not fatal -- the
  // player works, the keyboard's media keys do not.
  bool Start(void* native_window);

  // Closes the session and stops the timer. Idempotent, and called by the
  // destructor.
  void Stop();

  [[nodiscard]] std::string description() const;

 private:
  // One sample of the player, turned into at most three system calls. Called by
  // the timer, and again immediately after a command so the panel does not lag
  // its own button.
  void Observe();

  // On the UI thread, always: the platform delivers on whatever thread the
  // system used, and posts here.
  void Apply(const platform::MediaRequest& request);

  // Kept alive by the posted tasks. Its pointer is cleared by Stop(), on this
  // same thread, so a request that arrives during teardown finds nothing to
  // call rather than an object that is half gone.
  struct Inbox {
    ShellMediaSession* owner = nullptr;
  };

  PlayerHost& player_;
  LibraryHost& library_;
  std::unique_ptr<platform::MediaIntegration> integration_;
  core::MediaSessionPolicy policy_;
  std::shared_ptr<Inbox> inbox_;
  CefRefPtr<ShellTimer> timer_;
  // The cover already handed to the system, so the bytes are read from the
  // index once per album rather than once per track change.
  std::string art_key_;
};

}  // namespace sonora::shell
