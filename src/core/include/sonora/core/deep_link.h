#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace sonora::core {

// The URL scheme Sonora registers with the operating system.
//
// It is the same word the UI is served under (sonora://app/index.html), and
// that is deliberate rather than an accident waiting to happen: the host is
// what distinguishes them. "app" is served inside the browser by the scheme
// handler and never reaches the operating system; "track" and "album" only
// ever arrive from outside, as a command-line argument, because that is how
// Windows hands a registered scheme to a program. Two namespaces, one word,
// and no overlap -- but the rule has to be written down, because the day
// someone adds sonora://app as a deep link is the day the two meet.
inline constexpr std::string_view kUrlScheme = "sonora";

enum class DeepLinkKind {
  kInvalid,
  kTrack,  // sonora://track/<id> -- play that track
  kAlbum,  // sonora://album/<id> -- play the album that track belongs to
};

// The id is a **durable** id, issued by sonora::state -- not a row of the
// library index. That distinction is the whole reason the state store exists:
// index ids are reassigned every time the index is rebuilt, and a jump-list
// entry registered with Windows outlives several rebuilds. A URL that meant one
// song in March must not mean another in April.
//
// Why an album link names a track: albums have no identity of their own. ADR
// 0007 made them a GROUP BY rather than a table, so there is no album id to put
// in a URL, and inventing one for the jump list would mean inventing a second
// identity model for the sake of a context menu. Naming a track and saying "the
// album this belongs to" costs one word of explanation and no new concept.
struct DeepLink {
  DeepLinkKind kind = DeepLinkKind::kInvalid;
  // A durable id (sonora::state), resolved to a path and then to the index.
  std::int64_t track_id = 0;

  [[nodiscard]] bool valid() const noexcept { return kind != DeepLinkKind::kInvalid; }
  [[nodiscard]] bool operator==(const DeepLink& other) const noexcept {
    return kind == other.kind && track_id == other.track_id;
  }
};

// Parses a sonora:// URL. Never throws, never allocates, and answers kInvalid
// for everything it does not recognise.
//
// This is the one function in the project that reads a string written by
// somebody else entirely: a link in a web page, clicked by the user, handed to
// this process by Windows as an argument. It cannot name a file, and it cannot
// say anything but an integer -- the same rule the bridge got in week 7, for
// the same reason. Whatever comes out still has to exist in the index before it
// means anything.
[[nodiscard]] DeepLink ParseDeepLink(std::string_view url);

// The inverse, for the jump list: the entries it registers are URLs this
// process will be handed back later.
[[nodiscard]] std::string MakeDeepLink(DeepLinkKind kind, std::int64_t track_id);

}  // namespace sonora::core
