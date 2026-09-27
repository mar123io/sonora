#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sonora/update/hash.h"
#include "sonora/update/rollout.h"
#include "sonora/update/version.h"

namespace sonora::update {

// The manifest: the whole of the update server, as ADR 0009 decided.
//
// {
//   "schema": 1,
//   "channel": "stable",
//   "generated_at_ms": 1764460800000,
//   "releases": [
//     {
//       "version": "0.6.0",
//       "platform": "win-x64",
//       "archive":  { "size": 223426560, "hash": "<64 hex>" },
//       "package":  { "url": "https://.../Sonora-0.6.0-win-x64.spk.zst",
//                     "size": 49271217, "hash": "<64 hex>" },
//       "deltas":   [ { "from": "0.5.0", "url": "https://.../0.5.0-0.6.0.patch",
//                       "size": 86300, "hash": "<64 hex>" } ]
//     }
//   ]
// }
//
// "archive" has no url: it is the uncompressed archive, which is never served. Its
// size and hash are here because they are the preconditions for everything else --
// what a patch must produce, what a decompressed package must be, and what the
// client's own reconstruction of its installed version has to match before it is
// allowed to patch from it.
//
// This parser only ever sees bytes that have already been verified against
// ReleaseKeys(). That is not a reason for it to be lenient. It is a reason for it
// to be total: the process that signs the manifest is a workflow, and a workflow
// can be wrong.

inline constexpr int kManifestSchema = 1;
inline constexpr std::size_t kMaxManifestBytes = 1024 * 1024;
inline constexpr std::size_t kMaxReleases = 64;
inline constexpr std::size_t kMaxDeltasPerRelease = 32;
inline constexpr std::size_t kMaxUrlLength = 2048;

struct Artifact {
  std::string url;  // empty for the uncompressed archive, which is not served
  std::uint64_t size = 0;
  Hash256 hash{};
};

struct DeltaEntry {
  Version from;
  Artifact artifact;
};

struct Release {
  Version version;
  std::string platform;
  Artifact archive;  // uncompressed: size and hash only
  Artifact package;  // the same bytes, zstd-framed, with a url
  std::vector<DeltaEntry> deltas;
  // "rollout": { "percent": N }, absent meaning everybody. See ADR 0012: the beta
  // channel is a manifest that omits this, not a behaviour of the client.
  int rollout_percent = kFullRollout;
};

struct Manifest {
  int schema = 0;
  std::string channel;
  std::int64_t generated_at_ms = 0;
  std::vector<Release> releases;
};

enum class ManifestError {
  kNone,
  kTooLarge,
  kNotJson,
  kNotAnObject,
  kUnsupportedSchema,  // a manifest from the future: refused, never guessed at
  kBadChannel,
  kBadReleases,
  kBadVersion,
  kBadPlatform,
  kBadArtifact,
  kBadUrl,
  kBadHash,
  kBadDelta,
  kDuplicateRelease,
  kTooMany,
  kBadRollout,  // a percentage that is not one; refused rather than clamped
};

[[nodiscard]] std::string_view Describe(ManifestError error);

// Total. Never throws, and returns nothing for anything it does not fully
// understand.
//
// Unknown *fields* are ignored, unknown *schemas* are refused, and the difference
// is the contract: adding a field is how the manifest grows without breaking an
// installation from three years ago, so changing what an existing field means has
// to bump the schema. A parser that tolerated both would make the schema number
// decorative.
[[nodiscard]] std::optional<Manifest> ParseManifest(std::string_view json,
                                                    ManifestError& error);

struct UpdateTarget {
  Version version;
  Artifact package;  // the fallback, always present
  std::uint64_t archive_size = 0;
  Hash256 archive_hash{};
  // The patch from the version that is running, if this release publishes one.
  // Absent is normal: an installation older than the deltas the release kept, or
  // one whose own archive did not reconstruct, takes the package.
  std::optional<Artifact> delta;
};

// The newest release for this platform that is newer than `current` and is not in
// `refused`.
//
// `refused` is the journal's list of versions that were installed and could not
// start (ADR 0011). Skipping them here rather than at download time is what stops
// an installation from downloading, staging, failing and rolling back the same
// version once every six hours forever -- and it deliberately still offers an
// *older* refused-adjacent version if one is newer than current, because the
// judgement "this version does not start on this machine" is about one version and
// not about the channel.
// `install` is this installation's id, for the rollout arithmetic of ADR 0012. Absent --
// because the id could not be read or created -- means partial rollouts are declined: an
// installation that cannot work out where it stands is not in the first ten percent of
// anything. Releases at 100% are unaffected, which is every release before week 12.
[[nodiscard]] std::optional<UpdateTarget> ChooseUpdate(const Manifest& manifest,
                                                       std::string_view platform,
                                                       const Version& current,
                                                       std::span<const Version> refused,
                                                       const std::optional<InstallId>& install);

}  // namespace sonora::update
