#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <sonora/bridge/protocol.h>

// Which capabilities this run of the shell actually offers.
//
// A capability is a group of methods and events with a version. The page asks
// for the list before it does anything else and decides what to show from the
// answer, which is what lets a newer UI run against an older shell rather than
// throwing on the first method that is not there.
//
// Being able to switch one off at runtime is not a feature for users: it is how
// the degraded path gets exercised. A branch that only runs against a build
// from six months ago is a branch nobody tests.

namespace sonora::bridge {

// One capability as it stands right now: what the schema declared, plus
// whether this run has it on.
struct CapabilityState {
  std::string name;
  int version = 0;
  bool required = false;
  bool enabled = true;
};

class CapabilityRegistry {
 public:
  // The environment variable read by FromEnvironment: a comma-separated list
  // of capability names to switch off, e.g. SONORA_DISABLE_CAPS=diagnostics.
  static constexpr std::string_view kEnvironmentVariable = "SONORA_DISABLE_CAPS";

  // `declared` is the generated table; `disable_list` is the raw variable's
  // value, so the parsing has one implementation and the tests do not need an
  // environment.
  CapabilityRegistry(std::span<const CapabilityInfo> declared, std::string_view disable_list);

  // Everything enabled.
  explicit CapabilityRegistry(std::span<const CapabilityInfo> declared)
      : CapabilityRegistry(declared, std::string_view{}) {}

  [[nodiscard]] static CapabilityRegistry FromEnvironment(
      std::span<const CapabilityInfo> declared);

  [[nodiscard]] bool IsEnabled(std::string_view capability) const noexcept;

  // Throws BridgeError(kUnavailable) when the capability is off. Called by the
  // generated dispatch before every method, so "disabled" is one answer with
  // one error code rather than each handler's idea of what to do.
  void Require(std::string_view capability) const;

  [[nodiscard]] const std::vector<CapabilityState>& all() const noexcept { return state_; }

  // Names in the disable list that no capability matches. A typo in an
  // environment variable is silent by nature; this is what makes it visible.
  [[nodiscard]] const std::vector<std::string>& unknown() const noexcept { return unknown_; }

  // Names that were asked for and kept anyway, because they are required.
  [[nodiscard]] const std::vector<std::string>& refused() const noexcept { return refused_; }

  // One line for the startup log: what is on, what is off, what was ignored.
  [[nodiscard]] std::string Summary() const;

 private:
  std::vector<CapabilityState> state_;
  std::vector<std::string> unknown_;
  std::vector<std::string> refused_;
};

namespace detail {
// Exposed for its own test: splitting on commas and trimming is where an
// environment variable parser usually goes wrong.
[[nodiscard]] std::vector<std::string> SplitList(std::string_view text);
}  // namespace detail

}  // namespace sonora::bridge
