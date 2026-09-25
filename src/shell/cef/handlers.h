#pragma once

#include <bridge_generated.h>
#include <sonora/bridge/capabilities.h>

namespace sonora::shell {

class EventChannel;
class PlayerHost;
class ShellMetrics;

// The application's implementation of the generated bridge interface.
//
// There is no registration step and no table to keep in sync: BridgeHandlers is
// pure virtual, so adding a method to schema/bridge.schema.json stops the build
// here until it is implemented. That is the whole reason the interface is
// generated rather than looked up by name at runtime.
class ShellHandlers final : public bridge::BridgeHandlers {
 public:
  // All four must outlive the handlers. The runtime owns them and builds this
  // object last, which is the only ordering that works.
  ShellHandlers(const bridge::CapabilityRegistry& capabilities,
                const ShellMetrics& metrics,
                const EventChannel& events,
                PlayerHost& player);

  // shell
  bridge::ShellGetVersionResult ShellGetVersion(
      const bridge::ShellGetVersionParams& params) override;
  bridge::ShellEchoResult ShellEcho(const bridge::ShellEchoParams& params) override;
  bridge::ShellGetCapabilitiesResult ShellGetCapabilities(
      const bridge::ShellGetCapabilitiesParams& params) override;

  // player
  bridge::PlayerGetStateResult PlayerGetState(
      const bridge::PlayerGetStateParams& params) override;
  bridge::PlayerEnqueueResult PlayerEnqueue(const bridge::PlayerEnqueueParams& params) override;
  bridge::PlayerClearQueueResult PlayerClearQueue(
      const bridge::PlayerClearQueueParams& params) override;
  bridge::PlayerPlayResult PlayerPlay(const bridge::PlayerPlayParams& params) override;
  bridge::PlayerPauseResult PlayerPause(const bridge::PlayerPauseParams& params) override;
  bridge::PlayerStopResult PlayerStop(const bridge::PlayerStopParams& params) override;
  bridge::PlayerNextResult PlayerNext(const bridge::PlayerNextParams& params) override;
  bridge::PlayerPreviousResult PlayerPrevious(
      const bridge::PlayerPreviousParams& params) override;
  bridge::PlayerSeekResult PlayerSeek(const bridge::PlayerSeekParams& params) override;
  bridge::PlayerSetVolumeResult PlayerSetVolume(
      const bridge::PlayerSetVolumeParams& params) override;

  // diagnostics
  bridge::DiagnosticsGetMetricsResult DiagnosticsGetMetrics(
      const bridge::DiagnosticsGetMetricsParams& params) override;

 private:
  const bridge::CapabilityRegistry& capabilities_;
  const ShellMetrics& metrics_;
  const EventChannel& events_;
  PlayerHost& player_;
};

}  // namespace sonora::shell
