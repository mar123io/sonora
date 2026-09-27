#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace sonora::update {

// A binary patch, which is zstd compressing the new archive with the old one as a
// prefix -- what the zstd command line calls --patch-from.
//
// It is worth being precise about what that is, because "delta" suggests a list of
// differences and this is not one. The compressor is given the old file as
// dictionary-like context it may copy from, and then compresses the new file
// normally. A region that is unchanged compresses to "copy 137 MiB from offset 0",
// and a region that is new compresses as itself. There is no diffing pass, no
// alignment heuristic, and nothing that can be confused by bytes having shifted:
// see ADR 0010, where three lines of C++ moved 472 KiB of an executable and the
// patch was 84 KiB.
//
// The price is that the whole old file has to be in memory -- 213 MiB -- with a
// window large enough to reach across it, so applying a patch peaks around 430 MiB
// and generating one around 820 MiB. Measured, not guessed, and the reason the
// updater is a separate process that exits.

enum class PatchError {
  kNone,
  kUnknownSize,   // the patch does not declare what it produces; refused
  kSizeMismatch,  // it declares a size other than the one expected
  kTooLarge,
  kCorrupt,  // zstd said no
  kOutOfMemory,
  // The old and new payloads are the same bytes in memory. Refused, and this is
  // the one error here that exists because of a measurement rather than a
  // possibility: with the prefix and the input overlapping, zstd finds no matches
  // at all and returns a "patch" the size of the whole payload -- 262,159 bytes for
  // a 256 KiB input, against 36 for the same bytes in a second buffer. It does not
  // fail, it just quietly stops being a patch, which is the worst way for an
  // optimisation to break.
  kOverlappingInputs,
};

[[nodiscard]] std::string_view Describe(PatchError error);

// The level and the thread count the release workflow uses, here rather than in a
// script, so that the number that was measured and the number that runs are the
// same number.
//
// nbWorkers = 0 is the load-bearing one. Multithreaded compression splits the
// input into jobs compressed independently, and a patch is made of nothing but
// long-range matches -- so the threads that make compression fast are the threads
// that throw the matches away. On the measurement in ADR 0010 the same level
// produced 84.3 KiB single-threaded and 178.9 KiB with the tool's own default,
// and at level 17 it was 323.5 KiB: worse, and not even monotonic in the level.
inline constexpr int kPatchCompressionLevel = 19;
inline constexpr int kPatchWorkers = 0;

// The window has to reach back across the whole old archive or the matches that
// make a patch small are out of reach -- so it is computed from that size rather
// than fixed. A constant large enough for a 213 MiB payload would make every test
// in this file allocate 256 MiB to patch four kilobytes.
//
// The ceiling is 31 because that is the largest window zstd will build; an archive
// larger than 2 GiB would silently lose long-range matches, so the generator says
// so instead. The floor keeps the smallest windows out of pathological territory.
inline constexpr int kMinPatchWindowLog = 10;
inline constexpr int kMaxPatchWindowLog = 31;
[[nodiscard]] int PatchWindowLogFor(std::uint64_t old_size);

// Applies `patch` to `old_bytes`, expecting exactly `expected_size` bytes out.
//
// `expected_size` is not a hint, it is a precondition: it comes from the signed
// manifest, and a patch that declares any other size is refused before a single
// byte is allocated. This is the whole of this function's defence against a
// malicious patch, and it is why the manifest carries the uncompressed size of
// every archive.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> ApplyPatch(
    std::span<const std::uint8_t> old_bytes,
    std::span<const std::uint8_t> patch,
    std::uint64_t expected_size,
    PatchError& error);

// What a frame says it will produce, without producing it.
//
// The client never needs this: every size it uses comes from the signed manifest, which
// is the point. It exists for the release tooling, which has to expand a previous
// release's package in order to compute a delta against it and has no manifest to
// consult, because it is the thing that is about to write one.
[[nodiscard]] std::optional<std::uint64_t> DeclaredFrameSize(
    std::span<const std::uint8_t> frame);

// Decompresses a plain zstd frame -- the transport form of a package, which is the
// archive's bytes and nothing else (ADR 0010: compression is transport only).
//
// `expected_size` is again a precondition from the signed manifest rather than a
// hint, which is what keeps a 47 MiB download from being able to ask for an
// arbitrary allocation.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> DecompressFrame(
    std::span<const std::uint8_t> frame,
    std::uint64_t expected_size,
    PatchError& error);

// Compresses for transport. Not a patch; the level is high because this runs once
// per release on a runner and the result is what every installation downloads.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> CompressFrame(
    std::span<const std::uint8_t> bytes,
    PatchError& error,
    int level = kPatchCompressionLevel);

// Generates a patch. Used by tools/mkdelta and by the tests; never by the client,
// which only ever applies one.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> CreatePatch(
    std::span<const std::uint8_t> old_bytes,
    std::span<const std::uint8_t> new_bytes,
    PatchError& error,
    int level = kPatchCompressionLevel);

}  // namespace sonora::update
