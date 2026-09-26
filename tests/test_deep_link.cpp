#include <catch2/catch_test_macros.hpp>

#include <string>

#include <sonora/core/deep_link.h>

using namespace sonora::core;

TEST_CASE("a track link carries the id", "[deeplink]") {
  const DeepLink link = ParseDeepLink("sonora://track/42");
  REQUIRE(link.valid());
  REQUIRE(link.kind == DeepLinkKind::kTrack);
  REQUIRE(link.track_id == 42);
}

TEST_CASE("an album link names a track and means its album", "[deeplink]") {
  const DeepLink link = ParseDeepLink("sonora://album/7");
  REQUIRE(link.kind == DeepLinkKind::kAlbum);
  REQUIRE(link.track_id == 7);
}

TEST_CASE("the scheme and the host are case insensitive", "[deeplink]") {
  // Not politeness: a URL typed into a browser bar, or normalised by one, can
  // come back in any case, and the registry entry is matched case-insensitively
  // by Windows itself. Being stricter than the system that hands us the link
  // would only produce a link that works from one place and not another.
  REQUIRE(ParseDeepLink("SONORA://TRACK/3") == ParseDeepLink("sonora://track/3"));
  REQUIRE(ParseDeepLink("Sonora://Album/3").kind == DeepLinkKind::kAlbum);
}

TEST_CASE("a trailing slash is not a different link", "[deeplink]") {
  REQUIRE(ParseDeepLink("sonora://track/5/").track_id == 5);
}

TEST_CASE("a query string or a fragment is dropped, not refused", "[deeplink]") {
  // A link that survives a trip through a web page usually comes back with
  // something stuck to it.
  REQUIRE(ParseDeepLink("sonora://track/5?utm_source=whatever").track_id == 5);
  REQUIRE(ParseDeepLink("sonora://track/5#top").track_id == 5);
  REQUIRE(ParseDeepLink("sonora://track/5/?a=1#b").track_id == 5);
}

TEST_CASE("the UI's own host is not a deep link", "[deeplink]") {
  // sonora://app is served inside the browser and never comes from outside.
  // If it ever parses here, the two namespaces have met.
  REQUIRE_FALSE(ParseDeepLink("sonora://app/index.html").valid());
  REQUIRE_FALSE(ParseDeepLink("sonora://app/art/deadbeef").valid());
}

TEST_CASE("only this scheme, and only these hosts", "[deeplink]") {
  REQUIRE_FALSE(ParseDeepLink("https://track/1").valid());
  REQUIRE_FALSE(ParseDeepLink("sonorax://track/1").valid());
  REQUIRE_FALSE(ParseDeepLink("sonora://playlist/1").valid());
  REQUIRE_FALSE(ParseDeepLink("sonora://1").valid());
  REQUIRE_FALSE(ParseDeepLink("track/1").valid());
  REQUIRE_FALSE(ParseDeepLink("").valid());
  REQUIRE_FALSE(ParseDeepLink("sonora://").valid());
  REQUIRE_FALSE(ParseDeepLink("sonora://track/").valid());
}

TEST_CASE("the id is digits, and nothing that looks like digits", "[deeplink]") {
  // Every one of these is a way somebody has written "12" in a URL somewhere.
  REQUIRE_FALSE(ParseDeepLink("sonora://track/12abc").valid());
  REQUIRE_FALSE(ParseDeepLink("sonora://track/ 12").valid());
  REQUIRE_FALSE(ParseDeepLink("sonora://track/+12").valid());
  REQUIRE_FALSE(ParseDeepLink("sonora://track/-12").valid());
  REQUIRE_FALSE(ParseDeepLink("sonora://track/0x0c").valid());
  REQUIRE_FALSE(ParseDeepLink("sonora://track/1.0").valid());
  // Percent-encoded digits are not decoded on purpose: the only valid path is
  // digits, so decoding could never turn an invalid link into a valid one.
  REQUIRE_FALSE(ParseDeepLink("sonora://track/%31%32").valid());
  // And the reason that matters: %2F is a '/', and a parser that decoded first
  // would have to ask itself what the second segment means.
  REQUIRE_FALSE(ParseDeepLink("sonora://track/1%2F..%2Fetc").valid());
}

TEST_CASE("zero is not a track, it is a malformed link", "[deeplink]") {
  REQUIRE_FALSE(ParseDeepLink("sonora://track/0").valid());
}

TEST_CASE("an id larger than int64 is refused, not wrapped", "[deeplink]") {
  // The digits are all valid; the number is not. This is the case a hand-rolled
  // accumulate-and-multiply gets wrong silently, and it arrives from outside
  // the process.
  REQUIRE_FALSE(ParseDeepLink("sonora://track/99999999999999999999").valid());
  REQUIRE(ParseDeepLink("sonora://track/9223372036854775807").track_id == 9223372036854775807);
}

TEST_CASE("one segment, exactly", "[deeplink]") {
  REQUIRE_FALSE(ParseDeepLink("sonora://track/12/34").valid());
  REQUIRE_FALSE(ParseDeepLink("sonora://track/12/../../etc/passwd").valid());
}

TEST_CASE("an absurdly long argument is refused before it is parsed", "[deeplink]") {
  const std::string long_url = "sonora://track/" + std::string(4000, '1');
  REQUIRE_FALSE(ParseDeepLink(long_url).valid());
}

TEST_CASE("what the jump list writes is what this parses", "[deeplink]") {
  // The round trip is the actual contract: the entries registered with Windows
  // are URLs this same process is handed back, possibly weeks later, possibly
  // after an update.
  for (const DeepLinkKind kind : {DeepLinkKind::kTrack, DeepLinkKind::kAlbum}) {
    const std::string url = MakeDeepLink(kind, 1234);
    const DeepLink parsed = ParseDeepLink(url);
    REQUIRE(parsed.kind == kind);
    REQUIRE(parsed.track_id == 1234);
  }

  REQUIRE(MakeDeepLink(DeepLinkKind::kTrack, 0).empty());
  REQUIRE(MakeDeepLink(DeepLinkKind::kInvalid, 5).empty());
}
