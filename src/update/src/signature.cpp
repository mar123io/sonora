#include "sonora/update/signature.h"

#include <sodium.h>

#include <array>
#include <cstddef>

#include "sonora/update/hash.h"

namespace sonora::update {
namespace {

// The development key.
//
// Its private half was generated on a machine that is not a secret store, and it
// exists so that the tests, the local end-to-end run and the CI job have
// something real to verify against without a secret. **It must be replaced before
// the first release that anybody else installs**, because a signing key whose
// private half has ever been anywhere but a secret store is not a signing key.
//
// Replacing it is three commands, in tools/update_keygen.ps1:
//
//   openssl genpkey -algorithm ed25519 -out release.pem
//   openssl pkey -in release.pem -pubout -outform DER | ...last 32 bytes, hex...
//   gh secret set SONORA_RELEASE_KEY < release.pem
//
// ...and then this array holds the new key and the old one is deleted, not
// appended. The list in ReleaseKeys() exists for rotation between keys that are
// both protected; keeping a compromised key in it because removing things feels
// risky would make the list the weakest key in it.
//
// The `publish` job in .github/workflows/ci.yml greps for this fingerprint and
// refuses to publish a manifest while it is still here. A note saying "remember
// to change this" is not a check.
constexpr PublicKey kReleaseKey2026 = {
    0x05, 0x0c, 0x46, 0x24, 0x24, 0x4d, 0x10, 0x28, 0x16, 0x14, 0x5d,
    0x27, 0x60, 0x83, 0x97, 0xe3, 0x6c, 0x19, 0x9d, 0xff, 0xa9, 0xe2,
    0x94, 0x93, 0xe8, 0x05, 0xfa, 0x6b, 0xce, 0x47, 0x2e, 0x43,
};

constexpr std::array<PublicKey, 1> kKeys = {kReleaseKey2026};

template <std::size_t N>
std::optional<std::array<std::uint8_t, N>> ParseFixedHex(std::string_view hex) {
  if (hex.size() != N * 2) {
    return std::nullopt;
  }
  std::array<std::uint8_t, N> out{};
  for (std::size_t i = 0; i < N; ++i) {
    const auto digit = [](char c) -> int {
      if (c >= '0' && c <= '9') {
        return c - '0';
      }
      if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
      }
      return -1;
    };
    const int hi = digit(hex[i * 2]);
    const int lo = digit(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) {
      return std::nullopt;
    }
    out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
  }
  return out;
}

}  // namespace

std::optional<PublicKey> ParsePublicKey(std::string_view hex) {
  return ParseFixedHex<sizeof(PublicKey)>(hex);
}

std::optional<Signature> ParseSignature(std::string_view hex) {
  return ParseFixedHex<sizeof(Signature)>(hex);
}

bool VerifyDetached(std::span<const std::uint8_t> message,
                    const Signature& signature,
                    std::span<const PublicKey> keys) {
  if (!EnsureCryptoReady() || keys.empty()) {
    return false;
  }
  // A zero-length message is not a manifest, and signing nothing is not
  // something this project ever does. Refusing it here means the caller's
  // "download succeeded but returned an empty body" case cannot become "verified
  // an empty document".
  if (message.empty()) {
    return false;
  }
  for (const PublicKey& key : keys) {
    if (crypto_sign_verify_detached(signature.data(), message.data(), message.size(),
                                    key.data()) == 0) {
      return true;
    }
  }
  return false;
}

std::span<const PublicKey> ReleaseKeys() {
  return kKeys;
}

}  // namespace sonora::update
