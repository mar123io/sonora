#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace sonora::update {

// A release version: three numbers, nothing else.
//
// This is the same number cmake/Version.cmake reads out of the git tag, which is
// why the grammar here is exactly the grammar of the --match pattern there:
// vX.Y.Z and nothing more. The narrative tags (v0.4-native) are deliberately not
// versions, and this parser is where that stops being a convention and becomes a
// refusal.
//
// It is also a value that arrives from outside the process -- out of a manifest
// on the network, out of a journal file, off a command line -- so the parser is
// total and strict rather than forgiving. Strict in one particular way worth
// naming: leading zeros are refused, so that a version has exactly one spelling.
// If "1.02.0" parsed, two different strings would name the same release, and the
// refused list in the journal compares strings' meanings by comparing versions
// while the manifest compares them as keys. One spelling removes the question.
struct Version {
  std::uint32_t major = 0;
  std::uint32_t minor = 0;
  std::uint32_t patch = 0;

  [[nodiscard]] friend constexpr auto operator<=>(const Version&, const Version&) = default;
  [[nodiscard]] friend constexpr bool operator==(const Version&, const Version&) = default;

  [[nodiscard]] bool is_zero() const noexcept { return major == 0 && minor == 0 && patch == 0; }
};

// Parses "X.Y.Z". Returns nothing for anything else: leading or trailing spaces,
// a "v" prefix, a fourth component, a missing one, a leading zero, a value that
// does not fit in 32 bits.
[[nodiscard]] std::optional<Version> ParseVersion(std::string_view text);

// The inverse. ParseVersion(ToString(v)) == v for every Version.
[[nodiscard]] std::string ToString(const Version& version);

}  // namespace sonora::update
