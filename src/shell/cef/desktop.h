#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <sonora/platform/shell_integration.h>
#include <sonora/state/store.h>

#include "cef/timer.h"

namespace sonora::shell {

class LibraryHost;
class PlayerHost;

// Everything Sonora is outside its own window: the tray icon, the taskbar's
// thumbnail buttons, the jump list, and the links that arrive from elsewhere.
//
// It is the second object of its shape in this shell, after ShellMediaSession,
// and deliberately so -- both take the player and the library, turn them into
// something a platform integration can say, and turn what comes back into
// player commands. The alternative was one object doing both, and the moment
// the media panel and the tray disagreed about anything it would have needed
// splitting anyway.
//
// It also owns the durable store (ADR 0008), because everything that reads or
// writes it is here: what was played, and what a jump-list link means.
//
// Threading: the CEF UI thread owns all of it. Activations arrive on whatever
// thread the platform delivers them on and are posted here before anything is
// touched -- the same rule, and the same mechanism, as the media session.
class DesktopIntegration {
 public:
  DesktopIntegration(PlayerHost& player, LibraryHost& library);
  ~DesktopIntegration();

  DesktopIntegration(const DesktopIntegration&) = delete;
  DesktopIntegration& operator=(const DesktopIntegration&) = delete;

  // What the tray menu's "Show Sonora" and an incoming link need from the part
  // of the shell that owns the window.
  struct Callbacks {
    std::function<void()> raise_window;
    std::function<void()> quit;
  };

  // `store_path` is the durable store; it is created if it is not there. False
  // means the store could not be opened, and then nothing here runs: a jump
  // list built on ids that are not stable is worse than no jump list.
  bool Start(void* native_window, const std::filesystem::path& store_path, Callbacks callbacks);
  void Stop();

  [[nodiscard]] std::string description() const;

  // A command line -- this process's own at startup, or a second instance's,
  // forwarded. Anything in it that is not a sonora:// link is ignored here:
  // --play and --library are the launching process's business, not an
  // activation's.
  //
  // Safe to call from any thread.
  void Activate(const std::vector<std::string>& arguments);

 private:
  // On the UI thread, always.
  void ApplyActivation(const std::vector<std::string>& arguments);
  void ApplyCommand(platform::ShellCommand command);

  // One sample of the player: the tray's tooltip, and the play that has to be
  // written down. Runs on a timer, slowly -- none of this is animation.
  void Poll();

  void RecordPlay(const std::string& path);
  void RefreshJumpList();

  // Kept alive by posted tasks; cleared by Stop() so a click arriving during
  // teardown finds nothing rather than an object half gone. The same pattern as
  // ShellMediaSession::Inbox, and the same reason.
  struct Inbox {
    DesktopIntegration* owner = nullptr;
  };

  PlayerHost& player_;
  LibraryHost& library_;
  std::unique_ptr<state::Store> store_;
  std::unique_ptr<platform::ShellIntegration> integration_;
  std::shared_ptr<Inbox> inbox_;
  Callbacks callbacks_;
  CefRefPtr<ShellTimer> timer_;

  // What the tray was last told, so a tooltip is not rewritten four times a
  // minute for a string that has not changed.
  std::string last_tooltip_;
  bool last_playing_ = false;
  // The path the last recorded play was for.
  std::string recorded_path_;
};

}  // namespace sonora::shell
