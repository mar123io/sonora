#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "sonora/update/manifest.h"
#include "sonora/update/rollout.h"
#include "sonora/update/version.h"

namespace {

using sonora::update::ChooseUpdate;
using sonora::update::InRollout;
using sonora::update::InstallId;
using sonora::update::kFullRollout;
using sonora::update::Manifest;
using sonora::update::ManifestError;
using sonora::update::ParseInstallId;
using sonora::update::ParseManifest;
using sonora::update::RolloutBucket;
using sonora::update::Version;

// Install ids that are not random, so that a test about a distribution is about the
// distribution and not about today's seed.
InstallId Id(std::uint64_t n) {
  InstallId id{};
  for (std::size_t i = 0; i < 8; ++i) {
    id[i] = static_cast<std::uint8_t>((n >> (8 * i)) & 0xFF);
  }
  // The upper half is a constant that is not zero, so that Id(0) is not sixteen zero
  // bytes -- the one install id most likely to appear by accident.
  for (std::size_t i = 8; i < id.size(); ++i) {
    id[i] = 0x5A;
  }
  return id;
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

constexpr std::string_view kHash =
    "1111111111111111111111111111111111111111111111111111111111111111";

std::string ManifestWith(std::string_view rollout_for_0_6_1) {
  return std::string(R"json({
  "schema": 1,
  "channel": "stable",
  "releases": [
    {"version": "0.6.0", "platform": "win-x64",
     "archive": {"size": 10, "hash": ")json") +
         std::string(kHash) + R"json("},
     "package": {"url": "https://e.test/a", "size": 10, "hash": ")json" +
         std::string(kHash) + R"json("}},
    {"version": "0.6.1", "platform": "win-x64", )json" +
         std::string(rollout_for_0_6_1) + R"json(
     "archive": {"size": 20, "hash": ")json" +
         std::string(kHash) + R"json("},
     "package": {"url": "https://e.test/b", "size": 20, "hash": ")json" +
         std::string(kHash) + R"json("}}
  ]
})json";
}

}  // namespace

TEST_CASE("a bucket is always a hundredth, and always the same one") {
  for (std::uint64_t n = 0; n < 200; ++n) {
    const int bucket = RolloutBucket(Id(n), Version{1, 2, 3});
    CHECK(bucket >= 0);
    CHECK(bucket < kFullRollout);
    CHECK(bucket == RolloutBucket(Id(n), Version{1, 2, 3}));
  }
}

TEST_CASE("the buckets are spread over the hundred, not piled into a few") {
  // Ten thousand installations. Not a claim about cryptographic uniformity -- that is
  // BLAKE2b's business -- but a claim that the arithmetic around it did not collapse the
  // range, which is what an accidental `% 10` or a sign error looks like.
  std::array<int, kFullRollout> counts{};
  constexpr int kInstallations = 10000;
  for (std::uint64_t n = 0; n < kInstallations; ++n) {
    ++counts[static_cast<std::size_t>(RolloutBucket(Id(n), Version{0, 6, 1}))];
  }
  int lowest = kInstallations;
  int highest = 0;
  for (const int count : counts) {
    lowest = std::min(lowest, count);
    highest = std::max(highest, count);
  }
  // 100 expected per bucket. A binomial with n=10000, p=0.01 has a standard deviation of
  // about 10, so five sigma each way is a bound that will not flake and would still catch
  // a range collapsed to ten buckets or a bias towards the low ones.
  CHECK(lowest > 50);
  CHECK(highest < 150);
}

TEST_CASE("widening a rollout only ever adds installations") {
  // The property the version-salted bucket exists for: nobody is dropped out of a rollout
  // they were already in, so "we widened it and the crash reports stopped" means something.
  const Version version{0, 6, 1};
  std::set<std::uint64_t> previous;
  for (int percent = 0; percent <= kFullRollout; percent += 5) {
    std::set<std::uint64_t> included;
    for (std::uint64_t n = 0; n < 2000; ++n) {
      if (InRollout(Id(n), version, percent)) {
        included.insert(n);
      }
    }
    for (const std::uint64_t was_in : previous) {
      CHECK(included.count(was_in) == 1);
    }
    CHECK(included.size() >= previous.size());
    previous = std::move(included);
  }
  // ...and at 100 everybody is in.
  CHECK(previous.size() == 2000);
}

TEST_CASE("zero percent is nobody and a hundred is everybody") {
  for (std::uint64_t n = 0; n < 500; ++n) {
    CHECK_FALSE(InRollout(Id(n), Version{1, 0, 0}, 0));
    CHECK(InRollout(Id(n), Version{1, 0, 0}, kFullRollout));
  }
}

TEST_CASE("each version reshuffles who goes first") {
  // Being early for 0.6.1 says nothing about 0.6.2. Without this, the same machines would
  // be the early adopters of every release for as long as they own the computer -- which
  // turns a rollout into a permanent unpaid test group.
  int moved = 0;
  for (std::uint64_t n = 0; n < 1000; ++n) {
    if (RolloutBucket(Id(n), Version{0, 6, 1}) != RolloutBucket(Id(n), Version{0, 6, 2})) {
      ++moved;
    }
  }
  // Two independent draws from a hundred buckets agree about one time in a hundred, so
  // nearly everybody should have moved. Anything near zero would mean the version is not
  // reaching the hash.
  CHECK(moved > 950);
}

TEST_CASE("two installations are not the same installation") {
  int different = 0;
  for (std::uint64_t n = 0; n < 1000; ++n) {
    if (RolloutBucket(Id(n), Version{1, 0, 0}) != RolloutBucket(Id(n + 1), Version{1, 0, 0})) {
      ++different;
    }
  }
  CHECK(different > 950);
}

TEST_CASE("an install id round-trips through its own spelling") {
  for (std::uint64_t n = 0; n < 50; ++n) {
    const InstallId id = Id(n);
    const auto again = ParseInstallId(sonora::update::ToHex(id));
    REQUIRE(again.has_value());
    CHECK(*again == id);
  }
}

TEST_CASE("an install id has one spelling") {
  CHECK_FALSE(ParseInstallId("").has_value());
  CHECK_FALSE(ParseInstallId(std::string(30, 'a')).has_value());
  CHECK_FALSE(ParseInstallId(std::string(34, 'a')).has_value());
  CHECK_FALSE(ParseInstallId(std::string(32, 'A')).has_value());
  CHECK_FALSE(ParseInstallId(std::string(32, 'z')).has_value());
  CHECK(ParseInstallId(std::string(32, 'f')).has_value());
}

TEST_CASE("a new install id is random and sixteen bytes") {
  const auto first = sonora::update::NewInstallId();
  const auto second = sonora::update::NewInstallId();
  REQUIRE(first.has_value());
  REQUIRE(second.has_value());
  CHECK(first->size() == 16);
  CHECK(*first != *second);
  CHECK(*first != InstallId{});  // sixteen zero bytes is not a random number
}

TEST_CASE("the manifest carries the percentage, and absent means everybody") {
  const Manifest full = Parse(ManifestWith(""));
  REQUIRE(full.releases.size() == 2);
  for (const auto& release : full.releases) {
    CHECK(release.rollout_percent == kFullRollout);
  }

  const Manifest staged = Parse(ManifestWith(R"("rollout": {"percent": 10},)"));
  for (const auto& release : staged.releases) {
    if (release.version == Version{0, 6, 1}) {
      CHECK(release.rollout_percent == 10);
    } else {
      CHECK(release.rollout_percent == kFullRollout);
    }
  }
}

TEST_CASE("a percentage that is not one is refused, not clamped") {
  CHECK(ErrorFrom(ManifestWith(R"("rollout": {"percent": 101},)")) ==
        ManifestError::kBadRollout);
  CHECK(ErrorFrom(ManifestWith(R"("rollout": {"percent": -1},)")) ==
        ManifestError::kBadRollout);
  CHECK(ErrorFrom(ManifestWith(R"("rollout": {"percent": 1000},)")) ==
        ManifestError::kBadRollout);
  CHECK(ErrorFrom(ManifestWith(R"("rollout": {"percent": 10.5},)")) ==
        ManifestError::kBadRollout);
  CHECK(ErrorFrom(ManifestWith(R"("rollout": {"percent": "10"},)")) ==
        ManifestError::kBadRollout);
  CHECK(ErrorFrom(ManifestWith(R"("rollout": {},)")) == ManifestError::kBadRollout);
  CHECK(ErrorFrom(ManifestWith(R"("rollout": 10,)")) == ManifestError::kBadRollout);
  // ...and the two ends of the range are percentages.
  CHECK(Parse(ManifestWith(R"("rollout": {"percent": 0},)")).releases.size() == 2);
  CHECK(Parse(ManifestWith(R"("rollout": {"percent": 100},)")).releases.size() == 2);
}

TEST_CASE("a release held back from this installation does not hide an older one") {
  const Manifest manifest = Parse(ManifestWith(R"("rollout": {"percent": 10},)"));

  int offered_new = 0;
  int offered_old = 0;
  for (std::uint64_t n = 0; n < 1000; ++n) {
    const auto target = ChooseUpdate(manifest, "win-x64", Version{0, 5, 0}, {}, Id(n));
    REQUIRE(target.has_value());  // every installation gets *something*
    if (target->version == Version{0, 6, 1}) {
      ++offered_new;
    } else {
      CHECK(target->version == Version{0, 6, 0});
      ++offered_old;
    }
  }
  // About a tenth take 0.6.1 and the rest take 0.6.0, and nobody is told there is nothing.
  CHECK(offered_new > 50);
  CHECK(offered_new < 160);
  CHECK(offered_new + offered_old == 1000);
}

TEST_CASE("no install id declines a partial rollout rather than joining it") {
  const Manifest staged = Parse(ManifestWith(R"("rollout": {"percent": 10},)"));
  const auto target = ChooseUpdate(staged, "win-x64", Version{0, 5, 0}, {}, std::nullopt);
  REQUIRE(target.has_value());
  CHECK(target->version == Version{0, 6, 0});

  // A release at 100% is unaffected, which is every release before week 12.
  const Manifest full = Parse(ManifestWith(""));
  const auto everybody = ChooseUpdate(full, "win-x64", Version{0, 5, 0}, {}, std::nullopt);
  REQUIRE(everybody.has_value());
  CHECK(everybody->version == Version{0, 6, 1});
}

TEST_CASE("the rollout and the refused list are two filters, and both apply") {
  const Manifest manifest = Parse(ManifestWith(R"("rollout": {"percent": 100},)"));
  const Version refused[] = {Version{0, 6, 1}};

  for (std::uint64_t n = 0; n < 200; ++n) {
    // In the rollout, and refused: the older one.
    const auto target = ChooseUpdate(manifest, "win-x64", Version{0, 5, 0}, refused, Id(n));
    REQUIRE(target.has_value());
    CHECK(target->version == Version{0, 6, 0});
  }

  const Version both[] = {Version{0, 6, 1}, Version{0, 6, 0}};
  CHECK_FALSE(ChooseUpdate(manifest, "win-x64", Version{0, 5, 0}, both, Id(7)).has_value());
}

TEST_CASE("zero percent means nobody is offered it, whatever their id") {
  const Manifest manifest = Parse(ManifestWith(R"("rollout": {"percent": 0},)"));
  for (std::uint64_t n = 0; n < 500; ++n) {
    const auto target = ChooseUpdate(manifest, "win-x64", Version{0, 5, 0}, {}, Id(n));
    REQUIRE(target.has_value());
    CHECK(target->version == Version{0, 6, 0});
  }
}
