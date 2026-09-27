#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sonora/update/hash.h"
#include "sonora/update/manifest.h"
#include "sonora/update/signature.h"
#include "sonora/update/version.h"

namespace {

using sonora::update::ChooseUpdate;
using sonora::update::Manifest;
using sonora::update::ManifestError;
using sonora::update::ParseManifest;
using sonora::update::ParseVersion;
using sonora::update::Version;

// The manifest the release workflow produces, byte for byte, together with a real
// Ed25519 signature over exactly these bytes -- produced by `openssl pkeyutl
// -sign -rawin` with the development key whose public half is in signature.cpp.
//
// That crossing matters more than the test does: the release is signed by OpenSSL
// on a runner and verified by libsodium on a user's machine, and the only way to
// know those two agree about Ed25519 is to check a signature one of them made with
// the other. Editing a single character below breaks this test, which is the point.
constexpr std::string_view kManifest = R"json({
  "schema": 1,
  "channel": "stable",
  "generated_at_ms": 1764460800000,
  "releases": [
    {
      "version": "0.6.0",
      "platform": "win-x64",
      "archive": {
        "size": 223426560,
        "hash": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
      },
      "package": {
        "url": "https://mar123io.github.io/sonora/updates/stable/Sonora-0.6.0-win-x64.spk.zst",
        "size": 49271217,
        "hash": "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
      },
      "deltas": [
        {
          "from": "0.5.0",
          "url": "https://mar123io.github.io/sonora/updates/stable/0.5.0-0.6.0-win-x64.patch",
          "size": 86300,
          "hash": "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
        }
      ]
    }
  ]
})json";

constexpr std::string_view kManifestSignature =
    "829937c54edbabab5c30beea7ca94ee52eb35a545d549a9d1943f6456ef2c0f2"
    "63774c58dc17ccafbe5a8fac94e26efe9e29cdb8f5ef34c212a469087dfd730b";

std::span<const std::uint8_t> Bytes(std::string_view text) {
  return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

Manifest Parse(std::string_view json) {
  ManifestError error = ManifestError::kNone;
  const auto manifest = ParseManifest(json, error);
  REQUIRE(error == ManifestError::kNone);
  REQUIRE(manifest.has_value());
  return *manifest;
}

ManifestError ErrorFrom(std::string_view json) {
  ManifestError error = ManifestError::kNone;
  const auto manifest = ParseManifest(json, error);
  CHECK_FALSE(manifest.has_value());
  return error;
}

// Replaces the first occurrence of `from` with `to`, so that a case can say what
// it changed instead of repeating fifty lines of JSON.
std::string With(std::string_view from, std::string_view to) {
  std::string text(kManifest);
  const std::size_t at = text.find(from);
  REQUIRE(at != std::string::npos);
  text.replace(at, from.size(), to);
  return text;
}

}  // namespace

TEST_CASE("OpenSSL signs and libsodium verifies the same Ed25519") {
  const auto signature = sonora::update::ParseSignature(kManifestSignature);
  REQUIRE(signature.has_value());
  CHECK(sonora::update::VerifyDetached(Bytes(kManifest), *signature,
                                       sonora::update::ReleaseKeys()));
}

TEST_CASE("one byte of the manifest changed is a signature that does not verify") {
  const auto signature = sonora::update::ParseSignature(kManifestSignature);
  REQUIRE(signature.has_value());

  std::string tampered(kManifest);
  // The size of the package: the field an attacker who can rewrite the file but
  // not sign it would most like to change.
  const std::size_t at = tampered.find("49271217");
  REQUIRE(at != std::string::npos);
  tampered[at] = '5';

  CHECK_FALSE(sonora::update::VerifyDetached(Bytes(tampered), *signature,
                                             sonora::update::ReleaseKeys()));
}

TEST_CASE("an empty message never verifies, whatever the signature") {
  const auto signature = sonora::update::ParseSignature(kManifestSignature);
  REQUIRE(signature.has_value());
  CHECK_FALSE(sonora::update::VerifyDetached({}, *signature, sonora::update::ReleaseKeys()));
}

TEST_CASE("verification against no keys fails rather than succeeding vacuously") {
  const auto signature = sonora::update::ParseSignature(kManifestSignature);
  REQUIRE(signature.has_value());
  CHECK_FALSE(sonora::update::VerifyDetached(Bytes(kManifest), *signature, {}));
}

TEST_CASE("a signature is 64 bytes of hex and a key is 32") {
  CHECK_FALSE(sonora::update::ParseSignature("").has_value());
  CHECK_FALSE(sonora::update::ParseSignature(std::string(126, 'a')).has_value());
  CHECK_FALSE(sonora::update::ParseSignature(std::string(130, 'a')).has_value());
  CHECK_FALSE(sonora::update::ParseSignature(std::string(128, 'A')).has_value());
  CHECK(sonora::update::ParseSignature(std::string(128, 'a')).has_value());
  CHECK(sonora::update::ParsePublicKey(std::string(64, 'f')).has_value());
  CHECK_FALSE(sonora::update::ParsePublicKey(std::string(64, 'g')).has_value());
}

TEST_CASE("the manifest the workflow writes parses into what it says") {
  const Manifest manifest = Parse(kManifest);
  CHECK(manifest.schema == 1);
  CHECK(manifest.channel == "stable");
  CHECK(manifest.generated_at_ms == 1764460800000);
  REQUIRE(manifest.releases.size() == 1);

  const auto& release = manifest.releases.front();
  CHECK(release.version == Version{0, 6, 0});
  CHECK(release.platform == "win-x64");
  CHECK(release.archive.size == 223426560);
  CHECK(release.archive.url.empty());
  CHECK(release.package.size == 49271217);
  CHECK(release.package.url.ends_with("Sonora-0.6.0-win-x64.spk.zst"));
  REQUIRE(release.deltas.size() == 1);
  CHECK(release.deltas.front().from == Version{0, 5, 0});
  CHECK(release.deltas.front().artifact.size == 86300);
}

TEST_CASE("a schema from the future is refused, not read") {
  CHECK(ErrorFrom(With("\"schema\": 1", "\"schema\": 2")) == ManifestError::kUnsupportedSchema);
  CHECK(ErrorFrom(With("\"schema\": 1", "\"schema\": \"1\"")) ==
        ManifestError::kUnsupportedSchema);
  CHECK(ErrorFrom(With("\"schema\": 1,", "")) == ManifestError::kUnsupportedSchema);
}

TEST_CASE("an unknown field is ignored, because that is how a manifest grows") {
  const Manifest manifest = Parse(
      With("\"channel\": \"stable\"", "\"channel\": \"stable\",\n  \"rollout_percent\": 10"));
  CHECK(manifest.releases.size() == 1);
}

TEST_CASE("nothing but JSON gets in") {
  CHECK(ErrorFrom("") == ManifestError::kTooLarge);
  CHECK(ErrorFrom("{") == ManifestError::kNotJson);
  CHECK(ErrorFrom("[]") == ManifestError::kNotAnObject);
  CHECK(ErrorFrom("\"stable\"") == ManifestError::kNotAnObject);
  CHECK(ErrorFrom(std::string(2 * 1024 * 1024, 'x')) == ManifestError::kTooLarge);
}

TEST_CASE("a url must be https, and must not hide its host behind credentials") {
  constexpr std::string_view kPackageUrl =
      "https://mar123io.github.io/sonora/updates/stable/Sonora-0.6.0-win-x64.spk.zst";

  CHECK(ErrorFrom(With(kPackageUrl, "http://mar123io.github.io/sonora/x.spk.zst")) ==
        ManifestError::kBadUrl);
  CHECK(ErrorFrom(With(kPackageUrl, "https://downloads.sonora.app@evil.example/x")) ==
        ManifestError::kBadUrl);
  CHECK(ErrorFrom(With(kPackageUrl, "file:///C:/Sonora")) == ManifestError::kBadUrl);
  CHECK(ErrorFrom(With(kPackageUrl, "https://")) == ManifestError::kBadUrl);
  CHECK(ErrorFrom(With(kPackageUrl, "https:///x")) == ManifestError::kBadUrl);
  // A host that is not one. The characters are the cheap check; the reason for it
  // is that a manifest is read by a person before it is read by a program.
  CHECK(ErrorFrom(With(kPackageUrl, "https://-bad.example/x")) == ManifestError::kBadUrl);
  CHECK(ErrorFrom(With(kPackageUrl, "https://a..b/x")) == ManifestError::kBadUrl);
  CHECK(ErrorFrom(With(kPackageUrl, "https://a b/x")) == ManifestError::kBadUrl);
  CHECK(ErrorFrom(With(kPackageUrl, "https://[::1]/x")) == ManifestError::kBadUrl);

  // ...and the ones that are fine.
  CHECK(Parse(With(kPackageUrl, "https://example.test:8443/a/b.spk.zst")).releases.size() == 1);
  CHECK(Parse(With(kPackageUrl, "https://127.0.0.1/a.spk.zst")).releases.size() == 1);
}

TEST_CASE("the uncompressed archive has no url, because it is never served") {
  CHECK(ErrorFrom(With("\"size\": 223426560",
                       "\"url\": \"https://example.test/a\", \"size\": 223426560")) ==
        ManifestError::kBadArtifact);
}

TEST_CASE("a size of zero is not a size") {
  CHECK(ErrorFrom(With("\"size\": 49271217", "\"size\": 0")) == ManifestError::kBadArtifact);
  CHECK(ErrorFrom(With("\"size\": 49271217", "\"size\": -1")) == ManifestError::kBadArtifact);
  CHECK(ErrorFrom(With("\"size\": 49271217", "\"size\": \"49271217\"")) ==
        ManifestError::kBadArtifact);
}

TEST_CASE("a hash is 64 hex digits") {
  CHECK(ErrorFrom(With(std::string(64, 'b'), std::string(63, 'b'))) == ManifestError::kBadHash);
  CHECK(ErrorFrom(With(std::string(64, 'b'), std::string(64, 'B'))) == ManifestError::kBadHash);
  CHECK(ErrorFrom(With(std::string(64, 'b'), std::string(64, 'z'))) == ManifestError::kBadHash);
}

TEST_CASE("a delta must come from an older version") {
  CHECK(ErrorFrom(With("\"from\": \"0.5.0\"", "\"from\": \"0.6.0\"")) ==
        ManifestError::kBadDelta);
  CHECK(ErrorFrom(With("\"from\": \"0.5.0\"", "\"from\": \"0.7.0\"")) ==
        ManifestError::kBadDelta);
  CHECK(ErrorFrom(With("\"from\": \"0.5.0\"", "\"from\": \"v0.5.0\"")) ==
        ManifestError::kBadVersion);
}

TEST_CASE("a release without the pieces an update needs is not a release") {
  CHECK(ErrorFrom(With("\"platform\": \"win-x64\"", "\"platform\": \"Win_x64\"")) ==
        ManifestError::kBadPlatform);
  CHECK(ErrorFrom(With("\"version\": \"0.6.0\"", "\"version\": \"0.6\"")) ==
        ManifestError::kBadVersion);
  CHECK(ErrorFrom(R"json({"schema": 1, "channel": "stable", "releases": {}})json") ==
        ManifestError::kBadReleases);
  CHECK(ErrorFrom(R"json({"schema": 1, "channel": "stable"})json") ==
        ManifestError::kBadReleases);
  CHECK(ErrorFrom(R"json({"schema": 1, "channel": "stable", "releases": [7]})json") ==
        ManifestError::kBadReleases);
  // An empty channel is a manifest nobody asked for.
  CHECK(ErrorFrom(R"json({"schema": 1, "channel": "", "releases": []})json") ==
        ManifestError::kBadChannel);
}

namespace {

// Two platforms and three versions, for the choosing.
constexpr std::string_view kMulti = R"json({
  "schema": 1,
  "channel": "stable",
  "releases": [
    {"version": "0.5.0", "platform": "win-x64",
     "archive": {"size": 10, "hash": "1111111111111111111111111111111111111111111111111111111111111111"},
     "package": {"url": "https://e.test/a", "size": 10, "hash": "1111111111111111111111111111111111111111111111111111111111111111"}},
    {"version": "0.6.0", "platform": "win-x64",
     "archive": {"size": 20, "hash": "2222222222222222222222222222222222222222222222222222222222222222"},
     "package": {"url": "https://e.test/b", "size": 20, "hash": "2222222222222222222222222222222222222222222222222222222222222222"},
     "deltas": [
       {"from": "0.5.0", "url": "https://e.test/d5", "size": 5, "hash": "3333333333333333333333333333333333333333333333333333333333333333"},
       {"from": "0.4.0", "url": "https://e.test/d4", "size": 7, "hash": "4444444444444444444444444444444444444444444444444444444444444444"}
     ]},
    {"version": "0.7.0", "platform": "mac-arm64",
     "archive": {"size": 30, "hash": "5555555555555555555555555555555555555555555555555555555555555555"},
     "package": {"url": "https://e.test/c", "size": 30, "hash": "5555555555555555555555555555555555555555555555555555555555555555"}}
  ]
})json";

}  // namespace

TEST_CASE("choosing an update takes the newest for this platform") {
  const Manifest manifest = Parse(kMulti);
  const auto target = ChooseUpdate(manifest, "win-x64", Version{0, 4, 0}, {}, std::nullopt);
  REQUIRE(target.has_value());
  CHECK(target->version == Version{0, 6, 0});
  CHECK(target->package.url == "https://e.test/b");
  CHECK(target->archive_size == 20);
  REQUIRE(target->delta.has_value());
  CHECK(target->delta->url == "https://e.test/d4");
}

TEST_CASE("the delta chosen is the one from the version that is running") {
  const Manifest manifest = Parse(kMulti);
  const auto target = ChooseUpdate(manifest, "win-x64", Version{0, 5, 0}, {}, std::nullopt);
  REQUIRE(target.has_value());
  REQUIRE(target->delta.has_value());
  CHECK(target->delta->url == "https://e.test/d5");
}

TEST_CASE("no delta from this version means the full package, not no update") {
  const Manifest manifest = Parse(kMulti);
  const auto target = ChooseUpdate(manifest, "win-x64", Version{0, 3, 0}, {}, std::nullopt);
  REQUIRE(target.has_value());
  CHECK(target->version == Version{0, 6, 0});
  CHECK_FALSE(target->delta.has_value());
}

TEST_CASE("another platform's releases are not offered") {
  const Manifest manifest = Parse(kMulti);
  const auto target = ChooseUpdate(manifest, "win-x64", Version{0, 6, 0}, {}, std::nullopt);
  CHECK_FALSE(target.has_value());
  const auto mac = ChooseUpdate(manifest, "mac-arm64", Version{0, 6, 0}, {}, std::nullopt);
  REQUIRE(mac.has_value());
  CHECK(mac->version == Version{0, 7, 0});
  const auto unknown = ChooseUpdate(manifest, "linux-x64", Version{0, 1, 0}, {}, std::nullopt);
  CHECK_FALSE(unknown.has_value());
}

TEST_CASE("the same version is not an update, and an older one is never a downgrade") {
  const Manifest manifest = Parse(kMulti);
  CHECK_FALSE(
      ChooseUpdate(manifest, "win-x64", Version{0, 6, 0}, {}, std::nullopt).has_value());
  CHECK_FALSE(
      ChooseUpdate(manifest, "win-x64", Version{9, 0, 0}, {}, std::nullopt).has_value());
}

TEST_CASE("a refused version is skipped, and an older one is still offered") {
  const Manifest manifest = Parse(kMulti);
  const Version refused[] = {Version{0, 6, 0}};
  const auto target =
      ChooseUpdate(manifest, "win-x64", Version{0, 4, 0}, refused, std::nullopt);
  REQUIRE(target.has_value());
  // 0.6.0 would not start on this machine, so the newest one left that is still
  // newer than what is running is 0.5.0. Refusing a version is a judgement about
  // that version, not about the channel.
  CHECK(target->version == Version{0, 5, 0});

  const Version both[] = {Version{0, 6, 0}, Version{0, 5, 0}};
  CHECK_FALSE(
      ChooseUpdate(manifest, "win-x64", Version{0, 4, 0}, both, std::nullopt).has_value());
}

TEST_CASE("the same version and platform twice is a generator bug and is refused") {
  constexpr std::string_view kTwice = R"json({
  "schema": 1,
  "channel": "stable",
  "releases": [
    {"version": "0.6.0", "platform": "win-x64",
     "archive": {"size": 1, "hash": "1111111111111111111111111111111111111111111111111111111111111111"},
     "package": {"url": "https://e.test/a", "size": 1, "hash": "1111111111111111111111111111111111111111111111111111111111111111"}},
    {"version": "0.6.0", "platform": "win-x64",
     "archive": {"size": 2, "hash": "2222222222222222222222222222222222222222222222222222222222222222"},
     "package": {"url": "https://e.test/b", "size": 2, "hash": "2222222222222222222222222222222222222222222222222222222222222222"}}
  ]
})json";
  CHECK(ErrorFrom(kTwice) == ManifestError::kDuplicateRelease);

  // The same version for two platforms is not a duplicate; it is a release.
  std::string different(kTwice);
  const std::size_t at = different.rfind("win-x64");
  REQUIRE(at != std::string::npos);
  different.replace(at, std::string_view("win-x64").size(), "mac-x64");
  CHECK(Parse(different).releases.size() == 2);

  // The same delta source twice in one release, which is the other way a generator
  // can contradict itself.
  CHECK(ErrorFrom(R"json({"schema": 1, "channel": "stable", "releases": [
    {"version": "0.6.0", "platform": "win-x64",
     "archive": {"size": 1, "hash": "1111111111111111111111111111111111111111111111111111111111111111"},
     "package": {"url": "https://e.test/a", "size": 1, "hash": "1111111111111111111111111111111111111111111111111111111111111111"},
     "deltas": [
       {"from": "0.5.0", "url": "https://e.test/d1", "size": 1, "hash": "1111111111111111111111111111111111111111111111111111111111111111"},
       {"from": "0.5.0", "url": "https://e.test/d2", "size": 2, "hash": "2222222222222222222222222222222222222222222222222222222222222222"}
     ]}]})json") == ManifestError::kBadDelta);
}
