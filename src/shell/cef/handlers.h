#pragma once

#include <bridge_generated.h>

namespace sonora::shell {

// The application's implementation of the generated bridge interface.
//
// There is no registration step and no table to keep in sync: BridgeHandlers is
// pure virtual, so adding a method to schema/bridge.schema.json stops the build
// here until it is implemented. That is the whole reason the interface is
// generated rather than looked up by name at runtime.
class ShellHandlers final : public bridge::BridgeHandlers {
 public:
  ShellHandlers() = default;

  bridge::ShellGetVersionResult ShellGetVersion(
      const bridge::ShellGetVersionParams& params) override;

  bridge::ShellEchoResult ShellEcho(const bridge::ShellEchoParams& params) override;

  bridge::ShellListCapabilitiesResult ShellListCapabilities(
      const bridge::ShellListCapabilitiesParams& params) override;
};

}  // namespace sonora::shell
