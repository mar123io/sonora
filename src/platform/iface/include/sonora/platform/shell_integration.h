#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sonora::platform {

// What the tray menu and the taskbar's thumbnail buttons can ask for.
//
// The same vocabulary the media integration uses, and deliberately so: these
// are three different surfaces -- a system media panel, a notification-area
// menu, three buttons under a taskbar preview -- asking for the same six
// things. Giving each its own enum would mean three translations into the
// player's commands instead of one.
enum class ShellCommand {
  kShowWindow,
  kTogglePlayPause,
  kNext,
  kPrevious,
  kQuit,
};

using ShellCommandFn = std::function<void(ShellCommand)>;

// One entry in the jump list: what the user sees, and what launching it means.
//
// `arguments` is the command line the entry launches this same executable with
// -- a sonora:// URL. It is not a path, and it is not an index id: see ADR
// 0008. A jump list outlives the process that registered it, so what it holds
// has to outlive it too.
struct JumpListEntry {
  std::string title;        // the album
  std::string description;  // the artist, shown as a tooltip
  std::string arguments;
};

// What the tray icon says about the current state.
struct ShellState {
  std::string tooltip;  // "Sonora -- Artist - Title", or just "Sonora"
  bool playing = false;
};

// Tray icon, taskbar thumbnail buttons and jump list: the three places Windows
// expects a media application to show up outside its own window.
//
// All of it is optional, none of it is fatal, and every method is safe to call
// on a platform that implements none of them. A tray icon that fails to appear
// is a missing convenience; treating it as a startup failure would be a much
// worse bug than the one it reports.
class ShellIntegration {
 public:
  virtual ~ShellIntegration() = default;

  ShellIntegration(const ShellIntegration&) = delete;
  ShellIntegration& operator=(const ShellIntegration&) = delete;

  // `native_window` is the application window (HWND on Windows): the thumbnail
  // buttons belong to its taskbar button, and the tray menu's "show" raises it.
  // False means this platform has no shell integration -- not an error.
  virtual bool Start(void* native_window, ShellCommandFn on_command) = 0;
  virtual void Stop() = 0;

  virtual void SetState(const ShellState& state) = 0;

  // Replaces the whole list. Rebuilt rather than edited because the shell's own
  // API works that way, and because a list assembled from a durable store is
  // cheap enough that keeping a diff would be more code than it saves.
  virtual void SetJumpList(const std::vector<JumpListEntry>& entries) = 0;

  [[nodiscard]] virtual std::string description() const = 0;

 protected:
  ShellIntegration() = default;
};

// Never null: platforms without an implementation return one that does nothing
// and says so, so the shell has no branch.
[[nodiscard]] std::unique_ptr<ShellIntegration> CreateShellIntegration();

}  // namespace sonora::platform
