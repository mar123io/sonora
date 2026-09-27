#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace sonora::update {

// Ed25519. A 32-byte public key, a 64-byte detached signature.
//
// The whole of Sonora's trust in an update rests on these two numbers and on
// ADR 0009's rule: the signature covers the bytes of the manifest as they
// arrived, verified before the JSON parser is allowed to look at them. Every
// other artefact is authenticated by a hash that the signed manifest carries.
using PublicKey = std::array<std::uint8_t, 32>;
using Signature = std::array<std::uint8_t, 64>;

[[nodiscard]] std::optional<PublicKey> ParsePublicKey(std::string_view hex);
[[nodiscard]] std::optional<Signature> ParseSignature(std::string_view hex);

// True only if `signature` is a valid signature over `message` by one of `keys`.
//
// A list of keys, not a key, and the reason is a deadlock rather than a
// convenience. Rotating the release key means shipping a version that trusts the
// new one, and that version has to be delivered by an update signed with a key
// the installed copy already trusts. An installation that knows one key can never
// be moved to another; an installation that knows two can. Key n+1 is published
// in the release before it is used.
//
// Verification failure is not an error condition worth distinguishing from a
// malformed signature: both mean "this did not come from us", and telling them
// apart would only tell an attacker which half to work on.
[[nodiscard]] bool VerifyDetached(std::span<const std::uint8_t> message,
                                  const Signature& signature,
                                  std::span<const PublicKey> keys);

// The keys this build trusts, in the order they were introduced.
//
// They are here, in a header, on purpose: a reviewer can compare this line
// against the public key printed by tools/update_keygen.py and against the one in
// the workflow, which is the only check that ever catches a key being swapped.
// The private halves live in a repository secret and nowhere in this tree -- see
// ADR 0009, including what that costs.
[[nodiscard]] std::span<const PublicKey> ReleaseKeys();

}  // namespace sonora::update
