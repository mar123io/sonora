#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <random>
#include <vector>

#include "sonora/update/patch.h"

namespace {

using sonora::update::ApplyPatch;
using sonora::update::CreatePatch;
using sonora::update::PatchError;

// Incompressible bytes, so that the size of a patch says something about the patch
// and not about how well the payload happens to compress. A fixed seed, because a
// test whose subject is a compression ratio must not have a different subject on
// every run.
std::vector<std::uint8_t> Noise(std::size_t size, std::uint32_t seed) {
  std::mt19937 engine(seed);
  std::vector<std::uint8_t> bytes(size);
  for (std::uint8_t& byte : bytes) {
    byte = static_cast<std::uint8_t>(engine() & 0xFF);
  }
  return bytes;
}

}  // namespace

TEST_CASE("a patch between two versions of a payload rebuilds the second exactly") {
  const std::vector<std::uint8_t> old_bytes = Noise(512 * 1024, 1);

  // A change in the middle, and eight bytes inserted after it, so that everything
  // downstream shifts -- the case that makes a naive block-by-block diff useless
  // and that ADR 0010 measured on a real executable.
  std::vector<std::uint8_t> new_bytes = old_bytes;
  for (std::size_t i = 200000; i < 201000; ++i) {
    new_bytes[i] = static_cast<std::uint8_t>(i & 0xFF);
  }
  new_bytes.insert(new_bytes.begin() + 201000, 8, 0xEE);

  PatchError error = PatchError::kNone;
  const auto patch = CreatePatch(old_bytes, new_bytes, error);
  REQUIRE(error == PatchError::kNone);
  REQUIRE(patch.has_value());

  // The whole point: a patch is the size of what changed, not of what shifted.
  CHECK(patch->size() < 8 * 1024);

  const auto rebuilt = ApplyPatch(old_bytes, *patch, new_bytes.size(), error);
  REQUIRE(error == PatchError::kNone);
  REQUIRE(rebuilt.has_value());
  CHECK(*rebuilt == new_bytes);
}

TEST_CASE("a patch between identical payloads is almost nothing") {
  const std::vector<std::uint8_t> bytes = Noise(256 * 1024, 2);
  const std::vector<std::uint8_t> same_bytes = bytes;  // a second buffer, see below
  PatchError error = PatchError::kNone;
  const auto patch = CreatePatch(bytes, same_bytes, error);
  REQUIRE(patch.has_value());
  CHECK(patch->size() < 1024);
  const auto rebuilt = ApplyPatch(bytes, *patch, bytes.size(), error);
  REQUIRE(rebuilt.has_value());
  CHECK(*rebuilt == bytes);
}

TEST_CASE("generating a patch from a payload against itself is refused") {
  // Not a hypothetical. The first version of the test above passed the same vector
  // as both arguments, and zstd answered with a 262,159-byte "patch" for a 256 KiB
  // input: with the prefix overlapping the input it finds no matches at all. It
  // does not fail -- it stops being a patch, and the only symptom is a number in a
  // build log that nobody reads. A caller that memory-maps one file and hands the
  // same mapping to both arguments would ship full-size deltas forever.
  const std::vector<std::uint8_t> bytes = Noise(64 * 1024, 11);
  PatchError error = PatchError::kNone;
  CHECK_FALSE(CreatePatch(bytes, bytes, error).has_value());
  CHECK(error == PatchError::kOverlappingInputs);

  // A sub-span of the same buffer overlaps too.
  CHECK_FALSE(CreatePatch(bytes, std::span(bytes).subspan(1024), error).has_value());
  CHECK(error == PatchError::kOverlappingInputs);

  // Two buffers holding the same bytes do not.
  const std::vector<std::uint8_t> copy = bytes;
  CHECK(CreatePatch(bytes, copy, error).has_value());
}

TEST_CASE("a patch against the wrong old payload does not produce the right new one") {
  const std::vector<std::uint8_t> old_bytes = Noise(64 * 1024, 3);
  const std::vector<std::uint8_t> other = Noise(64 * 1024, 4);
  std::vector<std::uint8_t> new_bytes = old_bytes;
  new_bytes[100] ^= 0xFF;

  PatchError error = PatchError::kNone;
  const auto patch = CreatePatch(old_bytes, new_bytes, error);
  REQUIRE(patch.has_value());

  // This is the case the reconstruction hash check in the client exists to
  // prevent, and it is worth knowing exactly what happens without it: applying a
  // patch to the wrong input does not fail, it produces garbage of the right
  // length. Nothing about a patch can detect that on its own -- which is why the
  // client checks the hash of its reconstructed archive *before* patching, and the
  // hash of the result afterwards.
  const auto rebuilt = ApplyPatch(other, *patch, new_bytes.size(), error);
  if (rebuilt.has_value()) {
    CHECK(*rebuilt != new_bytes);
  }
}

TEST_CASE("a patch that declares a different size from the manifest is refused") {
  const std::vector<std::uint8_t> old_bytes = Noise(4096, 5);
  std::vector<std::uint8_t> new_bytes = old_bytes;
  new_bytes.push_back(0x11);

  PatchError error = PatchError::kNone;
  const auto patch = CreatePatch(old_bytes, new_bytes, error);
  REQUIRE(patch.has_value());

  const auto rebuilt = ApplyPatch(old_bytes, *patch, new_bytes.size() + 1, error);
  CHECK_FALSE(rebuilt.has_value());
  CHECK(error == PatchError::kSizeMismatch);

  CHECK_FALSE(ApplyPatch(old_bytes, *patch, 0, error).has_value());
  CHECK(error == PatchError::kTooLarge);
}

TEST_CASE("a corrupt patch is refused rather than applied") {
  const std::vector<std::uint8_t> old_bytes = Noise(8192, 6);
  std::vector<std::uint8_t> new_bytes = old_bytes;
  new_bytes[42] ^= 0xFF;

  PatchError error = PatchError::kNone;
  const auto patch = CreatePatch(old_bytes, new_bytes, error);
  REQUIRE(patch.has_value());
  REQUIRE(patch->size() > 16);

  // Every single byte of the patch, flipped, one at a time. Most flips land in the
  // compressed payload and are caught by zstd's checksum; a few land in the frame
  // header and are caught by the size check. The assertion is that none of them
  // produces the expected bytes.
  for (std::size_t i = 0; i < patch->size(); ++i) {
    std::vector<std::uint8_t> broken = *patch;
    broken[i] ^= 0x80;
    const auto rebuilt = ApplyPatch(old_bytes, broken, new_bytes.size(), error);
    if (rebuilt.has_value()) {
      // Corruption that happens to produce a valid frame of the right length must
      // still not produce the right contents -- and if it did, the caller's hash
      // check is the thing that catches it.
      CHECK((*rebuilt != new_bytes || broken == *patch));
    }
  }
}

TEST_CASE("truncating a patch is refused") {
  const std::vector<std::uint8_t> old_bytes = Noise(8192, 7);
  std::vector<std::uint8_t> new_bytes = old_bytes;
  new_bytes[99] ^= 0xFF;

  PatchError error = PatchError::kNone;
  const auto patch = CreatePatch(old_bytes, new_bytes, error);
  REQUIRE(patch.has_value());

  for (std::size_t length = 0; length < patch->size(); ++length) {
    const auto rebuilt =
        ApplyPatch(old_bytes, std::span(*patch).first(length), new_bytes.size(), error);
    CHECK_FALSE(rebuilt.has_value());
  }
}

TEST_CASE("bytes appended after a complete patch are refused") {
  const std::vector<std::uint8_t> old_bytes = Noise(4096, 8);
  std::vector<std::uint8_t> new_bytes = old_bytes;
  new_bytes[7] ^= 0xFF;

  PatchError error = PatchError::kNone;
  auto patch = CreatePatch(old_bytes, new_bytes, error);
  REQUIRE(patch.has_value());
  patch->push_back(0x00);

  CHECK_FALSE(ApplyPatch(old_bytes, *patch, new_bytes.size(), error).has_value());
  CHECK(error == PatchError::kCorrupt);
}

TEST_CASE("something that is not a patch at all") {
  const std::vector<std::uint8_t> old_bytes = Noise(1024, 9);
  const std::vector<std::uint8_t> nonsense = Noise(1024, 10);
  PatchError error = PatchError::kNone;
  CHECK_FALSE(ApplyPatch(old_bytes, nonsense, 1024, error).has_value());
  CHECK(error == PatchError::kCorrupt);
  CHECK_FALSE(ApplyPatch(old_bytes, {}, 1024, error).has_value());
}

TEST_CASE("the patch parameters are the ones that were measured") {
  // A test that asserts a constant is a tautology, and this one is here anyway, because
  // the mutation run found that nothing else in this file can see the difference.
  //
  // Setting nbWorkers to 4 changes nothing on the payloads a unit test can afford: zstd's
  // default job size is four times the window, so a 512 KiB input with a 512 KiB window
  // is one job whatever the thread count, and the output is identical. On the 213 MiB
  // payload of ADR 0010 the jobs are about 26 MiB, the long-range matches that a patch is
  // made of stop crossing them, and the patch goes from 84.3 KiB to 178.9 KiB.
  //
  // So the check on this number is the measurement in the ADR, not an assertion here, and
  // what this case buys is that changing it shows up in a diff that touches a test and
  // says why. That is worth three lines.
  CHECK(sonora::update::kPatchWorkers == 0);
  CHECK(sonora::update::kPatchCompressionLevel == 19);
}

TEST_CASE("the window is the size of the old payload, not a constant") {
  CHECK(sonora::update::PatchWindowLogFor(0) == sonora::update::kMinPatchWindowLog);
  CHECK(sonora::update::PatchWindowLogFor(1024) == 10);
  CHECK(sonora::update::PatchWindowLogFor(1025) == 11);
  CHECK(sonora::update::PatchWindowLogFor(223426560) == 28);
  // Larger than any window zstd will build: clamped, and the matches beyond it
  // are the price.
  CHECK(sonora::update::PatchWindowLogFor(1ull << 40) == sonora::update::kMaxPatchWindowLog);
}
