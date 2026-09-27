#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "sonora/update/version.h"

namespace sonora::update {

// Staged rollout, which is arithmetic and not a service. See ADR 0012.
//
// Sixteen random bytes, written once per installation and never transmitted, plus one
// hash, and an installation knows whether it is in the first ten percent of a release
// without asking anybody.

using InstallId = std::array<std::uint8_t, 16>;

// A new one, from the system's random source. Nothing about the machine goes into it: not
// the hostname, not a disk serial, not the user's name. What it identifies is an
// installation, so a reinstall getting a different one is correct rather than a flaw.
//
// Returns nothing if the random source is unavailable, which is a refusal and not a
// fallback: a predictable install id would put the same installations at the front of
// every rollout, which is exactly what ADR 0012 spends a field in the hash to avoid.
[[nodiscard]] std::optional<InstallId> NewInstallId();

[[nodiscard]] std::string ToHex(const InstallId& id);
[[nodiscard]] std::optional<InstallId> ParseInstallId(std::string_view hex);

inline constexpr int kFullRollout = 100;

// Which hundredth of the population this installation is in for this version.
//
// Always in [0, 100). A function of the installation *and* the version, which is what
// makes widening a rollout add installations without reshuffling them, and what stops the
// same machines from being the early adopters of every release forever. Both halves of
// that are the argument in ADR 0012.
[[nodiscard]] int RolloutBucket(const InstallId& id, const Version& version);

// Whether this installation takes a release published to `percent` of them.
//
// `percent` is 0..100 and is not clamped here: the manifest parser refuses anything else,
// so a value outside that range means the caller skipped the parser.
[[nodiscard]] bool InRollout(const InstallId& id, const Version& version, int percent);

}  // namespace sonora::update
