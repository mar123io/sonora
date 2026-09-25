#include "cef/handlers.h"

#include <filesystem>
#include <string>
#include <utility>

#include <sonora/audio/player.h>
#include <sonora/bridge/protocol.h>
#include <sonora/core/version.h>

#include "cef/event_channel.h"
#include "cef/player_host.h"
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

// The page asks the native side to open a file by name. That is a real
// boundary: the renderer is the untrusted half (ADR 0004), and "open whatever
// path this string says" is the shape of a great many security bugs.
//
// Week 6 accepts an absolute local path and nothing else. It is not a
// permission model -- a compromised renderer could still name any file the
// user can read -- and it is not pretending to be one; it rules out the
// mistakes rather than the attacks: a relative path resolved against whatever
// the working directory happens to be, and anything carrying a URL scheme.
//
// Week 7 removes the problem instead of guarding it: the library gives every
// track an id, the page sends the id, and no path ever crosses the bridge.
[[nodiscard]] std::filesystem::path ValidateTrackPath(const std::string& utf8) {
  if (utf8.empty()) {
    throw bridge::BridgeError(bridge::ErrorCode::kInvalidParams, "path must not be empty");
  }
  if (utf8.find("://") != std::string::npos) {
    throw bridge::BridgeError(bridge::ErrorCode::kInvalidParams,
                              "path must be a local file, not a URL");
  }

  const std::filesystem::path path(std::u8string(utf8.begin(), utf8.end()));
  if (!path.is_absolute()) {
    throw bridge::BridgeError(bridge::ErrorCode::kInvalidParams, "path must be absolute");
  }
  return path;
}

// Every transport command needs the same two lines, and forgetting them is a
// null dereference on a machine with no sound card.
[[nodiscard]] audio::Player& RequirePlayer(PlayerHost& host) {
  audio::Player* player = host.player();
  if (player == nullptr) {
    throw bridge::BridgeError(bridge::ErrorCode::kUnavailable, "no audio device is open");
  }
  return *player;
}

}  // namespace

ShellHandlers::ShellHandlers(const bridge::CapabilityRegistry& capabilities,
                             const ShellMetrics& metrics,
                             const EventChannel& events,
                             PlayerHost& player)
    : capabilities_(capabilities), metrics_(metrics), events_(events), player_(player) {}

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

// ---------------------------------------------------------------------------
// player
// ---------------------------------------------------------------------------
//
// Every command returns once the player has been told, not once the sound has
// changed. The two are different moments -- a seek takes as long as the decode
// thread needs -- and pretending otherwise would mean blocking a bridge call on
// audio, which is the one thing this whole design is arranged to avoid. The
// player.state event is what says what actually happened.

bridge::PlayerGetStateResult ShellHandlers::PlayerGetState(
    const bridge::PlayerGetStateParams& params) {
  (void)params;
  bridge::PlayerGetStateResult result;
  result.player = player_.State();
  return result;
}

bridge::PlayerEnqueueResult ShellHandlers::PlayerEnqueue(
    const bridge::PlayerEnqueueParams& params) {
  audio::Player& player = RequirePlayer(player_);
  player.Enqueue(ValidateTrackPath(params.path));

  // The queue size the page is told is the one it will see: Enqueue is applied
  // by the decode thread, so reading it back from the snapshot here would
  // report the value from before this call.
  bridge::PlayerEnqueueResult result;
  result.queueSize = player.snapshot().queue_size + 1;
  return result;
}

bridge::PlayerClearQueueResult ShellHandlers::PlayerClearQueue(
    const bridge::PlayerClearQueueParams& params) {
  (void)params;
  RequirePlayer(player_).ClearQueue();
  return {};
}

bridge::PlayerPlayResult ShellHandlers::PlayerPlay(const bridge::PlayerPlayParams& params) {
  (void)params;
  RequirePlayer(player_).Play();
  return {};
}

bridge::PlayerPauseResult ShellHandlers::PlayerPause(const bridge::PlayerPauseParams& params) {
  (void)params;
  RequirePlayer(player_).Pause();
  return {};
}

bridge::PlayerStopResult ShellHandlers::PlayerStop(const bridge::PlayerStopParams& params) {
  (void)params;
  RequirePlayer(player_).Stop();
  return {};
}

bridge::PlayerNextResult ShellHandlers::PlayerNext(const bridge::PlayerNextParams& params) {
  (void)params;
  RequirePlayer(player_).Next();
  return {};
}

bridge::PlayerPreviousResult ShellHandlers::PlayerPrevious(
    const bridge::PlayerPreviousParams& params) {
  (void)params;
  RequirePlayer(player_).Previous();
  return {};
}

bridge::PlayerSeekResult ShellHandlers::PlayerSeek(const bridge::PlayerSeekParams& params) {
  if (params.positionMs < 0) {
    throw bridge::BridgeError(bridge::ErrorCode::kInvalidParams,
                              "positionMs must not be negative");
  }
  RequirePlayer(player_).SeekMs(params.positionMs);
  return {};
}

bridge::PlayerSetVolumeResult ShellHandlers::PlayerSetVolume(
    const bridge::PlayerSetVolumeParams& params) {
  // Clamped rather than rejected: a slider that sends 1.0000001 because of
  // floating point is not a caller mistake worth an error.
  RequirePlayer(player_).SetVolume(static_cast<float>(params.level));
  return {};
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
