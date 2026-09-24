#include "cef/handlers.h"

#include <string>
#include <utility>

#include <sonora/bridge/protocol.h>
#include <sonora/core/version.h>

#include "cef/event_channel.h"
#include "cef/shell_metrics.h"
#include "include/cef_version.h"

namespace sonora::shell {
namespace {

// cef_version.h gives the parts and the stringify helper, but not the assembled
// Chromium version.
#define SONORA_CHROMIUM_VERSION                                                                \
  MAKE_STRING(CHROME_VERSION_MAJOR)                                                            \
  "." MAKE_STRING(CHROME_VERSION_MINOR) "." MAKE_STRING(CHROME_VERSION_BUILD) "." MAKE_STRING( \
      CHROME_VERSION_PATCH)

// A page asking for a million copies of a string is a page asking the browser
// process to allocate a gigabyte on its UI thread. The limit is the point.
constexpr std::int64_t kMaxEchoRepeat = 64;

}  // namespace

ShellHandlers::ShellHandlers(const bridge::CapabilityRegistry& capabilities,
                             const ShellMetrics& metrics,
                             const EventChannel& events)
    : capabilities_(capabilities), metrics_(metrics), events_(events) {}

bridge::ShellGetVersionResult ShellHandlers::ShellGetVersion(
    const bridge::ShellGetVersionParams& params) {
  (void)params;

  bridge::ShellGetVersionResult result;
  result.version = core::kVersion;
  result.gitDescribe = core::kGitDescribe;
  result.cefVersion = CEF_VERSION;
  result.chromiumVersion = SONORA_CHROMIUM_VERSION;
  return result;
}

bridge::ShellEchoResult ShellHandlers::ShellEcho(const bridge::ShellEchoParams& params) {
  // The generated parser has already checked the shapes. What is left is what
  // only this method knows: what the values are allowed to mean.
  if (params.message.empty()) {
    throw bridge::BridgeError(bridge::ErrorCode::kInvalidParams, "message must not be empty");
  }

  const std::int64_t repeat = params.repeat.value_or(1);
  if (repeat < 1 || repeat > kMaxEchoRepeat) {
    throw bridge::BridgeError(bridge::ErrorCode::kInvalidParams,
                              "repeat must be between 1 and " + std::to_string(kMaxEchoRepeat));
  }

  std::string repeated;
  repeated.reserve(params.message.size() * static_cast<std::size_t>(repeat));
  for (std::int64_t i = 0; i < repeat; ++i) {
    repeated += params.message;
  }

  bridge::ShellEchoResult result;
  result.lengthBytes = static_cast<std::int64_t>(repeated.size());
  result.message = std::move(repeated);
  return result;
}

bridge::ShellGetCapabilitiesResult ShellHandlers::ShellGetCapabilities(
    const bridge::ShellGetCapabilitiesParams& params) {
  (void)params;

  // Straight off the registry, which was built from the generated table. This
  // answer cannot disagree with what the schema declares or with what this run
  // has switched off, because there is no second list to keep in step.
  bridge::ShellGetCapabilitiesResult result;
  result.protocolVersion = bridge::kProtocolVersion;
  for (const bridge::CapabilityState& state : capabilities_.all()) {
    bridge::Capability capability;
    capability.name = state.name;
    capability.version = state.version;
    capability.enabled = state.enabled;
    result.capabilities.push_back(std::move(capability));
  }
  return result;
}

bridge::DiagnosticsGetMetricsResult ShellHandlers::DiagnosticsGetMetrics(
    const bridge::DiagnosticsGetMetricsParams& params) {
  (void)params;

  const bridge::EventCoalescer::Stats& events = events_.stats();

  bridge::DiagnosticsGetMetricsResult result;
  result.uptimeMs = metrics_.uptime_ms();
  result.queriesHandled = metrics_.queries_handled();
  result.eventsPosted = events.posted;
  result.eventsDelivered = events.delivered;
  result.eventsCoalesced = events.coalesced;
  return result;
}

}  // namespace sonora::shell
