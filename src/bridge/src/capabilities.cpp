#include <sonora/bridge/capabilities.h>

#include <algorithm>
#include <cstdlib>

namespace sonora::bridge {
namespace {

constexpr std::string_view kWhitespace = " \t\r\n";

[[nodiscard]] std::string_view Trim(std::string_view text) {
  const auto first = text.find_first_not_of(kWhitespace);
  if (first == std::string_view::npos) {
    return {};
  }
  const auto last = text.find_last_not_of(kWhitespace);
  return text.substr(first, last - first + 1);
}

}  // namespace

namespace detail {

std::vector<std::string> SplitList(std::string_view text) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start <= text.size()) {
    const auto comma = text.find(',', start);
    const auto piece = Trim(text.substr(
        start, comma == std::string_view::npos ? std::string_view::npos : comma - start));
    if (!piece.empty()) {
      out.emplace_back(piece);
    }
    if (comma == std::string_view::npos) {
      break;
    }
    start = comma + 1;
  }
  return out;
}

}  // namespace detail

CapabilityRegistry::CapabilityRegistry(std::span<const CapabilityInfo> declared,
                                       std::string_view disable_list) {
  state_.reserve(declared.size());
  for (const CapabilityInfo& info : declared) {
    state_.push_back(
        CapabilityState{std::string(info.name), info.version, info.required, true});
  }

  for (const std::string& name : detail::SplitList(disable_list)) {
    const auto found = std::find_if(state_.begin(), state_.end(),
                                    [&](const CapabilityState& s) { return s.name == name; });
    if (found == state_.end()) {
      unknown_.push_back(name);
      continue;
    }
    if (found->required) {
      // Switching off the capability that reports the capabilities leaves the
      // page with no way to find out what happened. Refused, and said out loud.
      refused_.push_back(name);
      continue;
    }
    found->enabled = false;
  }
}

CapabilityRegistry CapabilityRegistry::FromEnvironment(
    std::span<const CapabilityInfo> declared) {
#if defined(_MSC_VER)
  // MSVC deprecates std::getenv in favour of _dupenv_s. The warning is about
  // programs that also call putenv from another thread; this one never writes
  // the environment at all. Suppressed here rather than project-wide, and
  // narrowly: the rest of the file keeps the warning.
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
  const char* raw = std::getenv(std::string(kEnvironmentVariable).c_str());
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
  return CapabilityRegistry(declared,
                            raw == nullptr ? std::string_view{} : std::string_view(raw));
}

bool CapabilityRegistry::IsEnabled(std::string_view capability) const noexcept {
  const auto found = std::find_if(state_.begin(), state_.end(), [&](const CapabilityState& s) {
    return s.name == capability;
  });
  return found != state_.end() && found->enabled;
}

void CapabilityRegistry::Require(std::string_view capability) const {
  if (IsEnabled(capability)) {
    return;
  }
  // kUnavailable, not kUnknownMethod: the difference matters to the page. One
  // means "this shell is too old for you", the other means "you asked for
  // something that has never existed".
  throw BridgeError(ErrorCode::kUnavailable, "capability '" + std::string(capability) +
                                                 "' is not available in this run");
}

std::string CapabilityRegistry::Summary() const {
  std::string enabled;
  std::string disabled;
  for (const CapabilityState& state : state_) {
    std::string& target = state.enabled ? enabled : disabled;
    if (!target.empty()) {
      target += ", ";
    }
    target += state.name + "@" + std::to_string(state.version);
  }

  std::string out = "enabled: " + (enabled.empty() ? std::string("none") : enabled);
  if (!disabled.empty()) {
    out += "; disabled: " + disabled;
  }
  for (const std::string& name : refused_) {
    out += "; '" + name + "' cannot be disabled (required)";
  }
  for (const std::string& name : unknown_) {
    out += "; '" + name + "' is not a capability";
  }
  return out;
}

}  // namespace sonora::bridge
