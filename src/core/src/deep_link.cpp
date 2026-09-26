#include <sonora/core/deep_link.h>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <system_error>

namespace sonora::core {
namespace {

// Long enough for anything legitimate by three orders of magnitude. A cap at
// the front means no later step has to think about a megabyte of argument.
constexpr std::size_t kMaxUrlLength = 2048;

[[nodiscard]] bool EqualsIgnoringAsciiCase(std::string_view left, std::string_view right) {
  // Deliberately ASCII-only. A scheme and a host are ASCII by definition, and
  // a locale-aware comparison here would mean the Turkish locale's dotless i
  // decides whether a link works.
  if (left.size() != right.size()) {
    return false;
  }
  return std::equal(left.begin(), left.end(), right.begin(), [](char a, char b) {
    const char lower_a = (a >= 'A' && a <= 'Z') ? static_cast<char>(a - 'A' + 'a') : a;
    const char lower_b = (b >= 'A' && b <= 'Z') ? static_cast<char>(b - 'A' + 'a') : b;
    return lower_a == lower_b;
  });
}

[[nodiscard]] DeepLinkKind KindFromHost(std::string_view host) {
  if (EqualsIgnoringAsciiCase(host, "track")) {
    return DeepLinkKind::kTrack;
  }
  if (EqualsIgnoringAsciiCase(host, "album")) {
    return DeepLinkKind::kAlbum;
  }
  return DeepLinkKind::kInvalid;
}

// Digits only, and nothing else. No percent-decoding happens anywhere in this
// file, and that is a decision rather than an omission: the only path this
// scheme accepts is a run of decimal digits, so decoding could not make a valid
// link out of anything -- it would only add more ways to spell one that already
// works, and every extra spelling is another line to be wrong in. %2F cannot
// smuggle a path separator past a parser that never looks at %.
[[nodiscard]] bool ParseId(std::string_view text, std::int64_t& out) {
  if (text.empty()) {
    return false;
  }
  if (std::any_of(text.begin(), text.end(), [](char c) { return c < '0' || c > '9'; })) {
    return false;
  }

  std::int64_t value = 0;
  const auto* first = text.data();
  const auto* last = text.data() + text.size();
  const auto result = std::from_chars(first, last, value);
  // ec covers the case the digit check cannot: an id with more digits than an
  // int64 can hold. from_chars reports it instead of wrapping, which is the
  // whole reason it is here rather than std::stoll.
  if (result.ec != std::errc{} || result.ptr != last) {
    return false;
  }
  // Row ids start at 1. Zero is not a track that might be missing, it is a
  // malformed link, and the two deserve different answers.
  if (value <= 0) {
    return false;
  }
  out = value;
  return true;
}

}  // namespace

DeepLink ParseDeepLink(std::string_view url) {
  const DeepLink invalid;

  if (url.empty() || url.size() > kMaxUrlLength) {
    return invalid;
  }

  // Everything from the first '?' or '#' is somebody else's idea, not ours.
  // Dropping it rather than rejecting the link is what makes a URL that picked
  // up a tracking parameter on its way through a web page still work.
  url = url.substr(0, std::min(url.find('?'), url.find('#')));

  const std::size_t separator = url.find("://");
  if (separator == std::string_view::npos) {
    return invalid;
  }
  if (!EqualsIgnoringAsciiCase(url.substr(0, separator), kUrlScheme)) {
    return invalid;
  }

  std::string_view rest = url.substr(separator + 3);
  // A trailing slash is added by some shells and by some browsers, and a link
  // that works when typed and fails when clicked is not a distinction worth
  // defending.
  while (!rest.empty() && rest.back() == '/') {
    rest.remove_suffix(1);
  }

  const std::size_t slash = rest.find('/');
  if (slash == std::string_view::npos) {
    return invalid;
  }

  DeepLink link;
  link.kind = KindFromHost(rest.substr(0, slash));
  if (link.kind == DeepLinkKind::kInvalid) {
    return invalid;
  }

  // One segment, exactly. sonora://track/12/../../etc is not a link with an
  // interesting suffix, it is not a link.
  const std::string_view id = rest.substr(slash + 1);
  if (!ParseId(id, link.track_id)) {
    return invalid;
  }

  return link;
}

std::string MakeDeepLink(DeepLinkKind kind, std::int64_t track_id) {
  if (kind == DeepLinkKind::kInvalid || track_id <= 0) {
    return {};
  }
  std::string url(kUrlScheme);
  url += "://";
  url += (kind == DeepLinkKind::kTrack) ? "track/" : "album/";
  url += std::to_string(track_id);
  return url;
}

}  // namespace sonora::core
