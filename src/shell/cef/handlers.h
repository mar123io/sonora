#pragma once

#include <bridge_generated.h>
#include <sonora/bridge/capabilities.h>

namespace sonora::shell {

class EventChannel;
class ShellMetrics;

// The application's implementation of the generated bridge interface.
//
// There is no registration step and no table to keep in sync: BridgeHandlers is
// pure virtual, so adding a method to schema/bridge.schema.json stops the build
// here until it is implemented. That is the whole reason the interface is
// generated rather than looked up by name at runtime.
class ShellHandlers final : public bridge::BridgeHandlers {
 public:
  // All three must outlive the handlers. The runtime owns them and builds this
  // object last, which is the only ordering that works.
  ShellHandlers(const bridge::CapabilityRegistry& capabilities,
                const ShellMetrics& metrics,
                const EventChannel& events);

  bridge::ShellGetVersionResult ShellGetVersion(
      const bridge::ShellGetVersionParams& params) override;

  bridge::ShellEchoResult ShellEcho(const bridge::ShellEchoParams& params) override;

  bridge::ShellGetCapabilitiesResult ShellGetCapabilities(
      const bridge::ShellGetCapabilitiesParams& params) override;

  bridge::DiagnosticsGetMetricsResult DiagnosticsGetMetrics(
      const bridge::DiagnosticsGetMetricsParams& params) override;

 private:
  const bridge::CapabilityRegistry& capabilities_;
  const ShellMetrics& metrics_;
  const EventChannel& events_;
};

}  // namespace sonora::shell
