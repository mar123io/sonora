#include "sonora/update/version.h"

#include <charconv>
#include <cstddef>
#include <limits>

namespace sonora::update {
namespace {

// One component of a version: a run of decimal digits with no leading zero
// unless the whole thing is "0", and a value that fits in 32 bits.
//
// from_chars is what refuses the overflow. Doing the arithmetic by hand would
// mean writing the overflow check by hand, and the standard library already has
// one that is right.
bool ParseComponent(std::string_view text, std::uint32_t& out) {
  if (text.empty()) {
    return false;
  }
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
  }
  if (text.size() > 1 && text.front() == '0') {
    return false;
  }
  const char* const begin = text.data();
  const char* const end = begin + text.size();
  std::uint32_t value = 0;
  const auto result = std::from_chars(begin, end, value);
  if (result.ec != std::errc{} || result.ptr != end) {
    return false;
  }
  out = value;
  return true;
}

}  // namespace

std::optional<Version> ParseVersion(std::string_view text) {
  // A cap before anything else: this string arrives from a file or a network
  // response, and there is no version longer than thirty characters.
  if (text.empty() || text.size() > 32) {
    return std::nullopt;
  }

  const std::size_t first = text.find('.');
  if (first == std::string_view::npos) {
    return std::nullopt;
  }
  const std::size_t second = text.find('.', first + 1);
  if (second == std::string_view::npos) {
    return std::nullopt;
  }
  // A fourth component is not a version this project knows how to compare, so it
  // is refused rather than truncated.
  if (text.find('.', second + 1) != std::string_view::npos) {
    return std::nullopt;
  }

  Version version;
  if (!ParseComponent(text.substr(0, first), version.major) ||
      !ParseComponent(text.substr(first + 1, second - first - 1), version.minor) ||
      !ParseComponent(text.substr(second + 1), version.patch)) {
    return std::nullopt;
  }
  return version;
}

std::string ToString(const Version& version) {
  std::string out;
  out.reserve(11 * 3 + 2);
  out += std::to_string(version.major);
  out += '.';
  out += std::to_string(version.minor);
  out += '.';
  out += std::to_string(version.patch);
  return out;
}

}  // namespace sonora::update
