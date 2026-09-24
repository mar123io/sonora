#include "cef/handlers.h"

#include <string>

#include <sonora/bridge/protocol.h>
#include <sonora/core/version.h>

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

bridge::ShellListCapabilitiesResult ShellHandlers::ShellListCapabilities(
    const bridge::ShellListCapabilitiesParams& params) {
  (void)params;

  // Read straight off the generated table, so this answer cannot disagree with
  // what the schema says this build exposes.
  bridge::ShellListCapabilitiesResult result;
  for (const auto& capability : bridge::kCapabilities) {
    result.names.emplace_back(capability.name);
    result.versions.push_back(capability.version);
  }
  return result;
}

}  // namespace sonora::shell
