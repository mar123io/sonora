#include <sonora/platform/shell_integration.h>

// A shell integration that integrates with nothing.
//
// It exists so that the shell has no branch: a tray icon, a jump list and three
// buttons under a taskbar preview are conveniences, and a program that has to
// ask whether it is on Windows before deciding what to do with a missing
// convenience ends up with that question in five places.
//
// macOS is not served by this file forever -- a status-bar item and the Dock
// menu are the equivalents there, and they are a week of their own. Linux has
// no window backend at all yet, so there is nothing for an indicator to attach
// to. Both are honest gaps rather than hidden ones: description() says so, and
// the shell prints it at startup.

namespace sonora::platform {
namespace {

class NoShellIntegration final : public ShellIntegration {
 public:
  bool Start(void* native_window, ShellCommandFn on_command) override {
    (void)native_window;
    (void)on_command;
    return false;
  }

  void Stop() override {}

  void SetState(const ShellState& state) override { (void)state; }

  void SetJumpList(const std::vector<JumpListEntry>& entries) override { (void)entries; }

  [[nodiscard]] std::string description() const override {
    return "none (no tray, no jump list) on this platform";
  }
};

}  // namespace

std::unique_ptr<ShellIntegration> CreateShellIntegration() {
  return std::make_unique<NoShellIntegration>();
}

}  // namespace sonora::platform
