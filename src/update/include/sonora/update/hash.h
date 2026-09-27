#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace sonora::update {

// BLAKE2b truncated to 256 bits, which is what every hash in this subsystem
// means: the hashes in the manifest, the per-member hashes in the archive, the
// identity of a downloaded package.
//
// Why BLAKE2b and not SHA-256: it is what libsodium offers as its general-purpose
// hash, and libsodium is already here for the signature. One cryptographic
// dependency, one initialisation, one set of constant-time comparisons. Choosing
// SHA-256 for familiarity would mean either a second library or writing a second
// primitive, and ADR 0009 says this repository implements none.
using Hash256 = std::array<std::uint8_t, 32>;

// libsodium requires one initialisation before anything else, from one thread.
// Everything in this header and in signature.h calls it, so no caller has to
// remember; it is idempotent and safe to call from the same thread repeatedly.
// Returns false if libsodium itself reports that it cannot initialise, in which
// case nothing here will hash or verify anything -- the failure is returned
// rather than ignored, because a hash that silently is not one is worse than no
// hash at all.
[[nodiscard]] bool EnsureCryptoReady();

[[nodiscard]] std::optional<Hash256> HashBytes(std::span<const std::uint8_t> bytes);

// Incremental hashing, for the files that do not fit in memory -- which is most
// of them: the payload is 213 MiB and the largest single member is 137 MiB.
class Hasher {
 public:
  Hasher();
  ~Hasher();
  Hasher(const Hasher&) = delete;
  Hasher& operator=(const Hasher&) = delete;
  Hasher(Hasher&&) noexcept;
  Hasher& operator=(Hasher&&) noexcept;

  [[nodiscard]] bool ok() const noexcept { return state_ != nullptr; }
  void Update(std::span<const std::uint8_t> bytes);
  // Valid once. A second call returns nothing.
  [[nodiscard]] std::optional<Hash256> Finish();

 private:
  struct State;
  std::unique_ptr<State> state_;
};

// Lower-case hex, both directions. ParseHash refuses anything that is not
// exactly 64 hex digits -- no "0x", no upper case mixed in, no whitespace -- so
// that a hash has one spelling for the same reason a version does.
[[nodiscard]] std::string ToHex(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::optional<Hash256> ParseHash(std::string_view hex);

// Constant-time. Used everywhere two hashes are compared, including where the
// timing cannot matter, because a comparison that is sometimes constant-time and
// sometimes not is a rule nobody can check.
[[nodiscard]] bool HashesEqual(const Hash256& a, const Hash256& b);

}  // namespace sonora::update
