#include "sonora/update/rollout.h"

#include <sodium.h>

#include <cstddef>
#include <vector>

#include "sonora/update/hash.h"

namespace sonora::update {

std::optional<InstallId> NewInstallId() {
  if (!EnsureCryptoReady()) {
    return std::nullopt;
  }
  InstallId id{};
  randombytes_buf(id.data(), id.size());
  return id;
}

std::string ToHex(const InstallId& id) {
  return ToHex(std::span<const std::uint8_t>(id.data(), id.size()));
}

std::optional<InstallId> ParseInstallId(std::string_view hex) {
  if (hex.size() != sizeof(InstallId) * 2) {
    return std::nullopt;
  }
  InstallId id{};
  for (std::size_t i = 0; i < id.size(); ++i) {
    const auto digit = [](char c) -> int {
      if (c >= '0' && c <= '9') {
        return c - '0';
      }
      if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
      }
      return -1;
    };
    const int high = digit(hex[i * 2]);
    const int low = digit(hex[i * 2 + 1]);
    if (high < 0 || low < 0) {
      return std::nullopt;
    }
    id[i] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return id;
}

int RolloutBucket(const InstallId& id, const Version& version) {
  // The version's spelling and not its three numbers, because ToString is the one
  // spelling a version has (see version.h) and because the workflow that writes the
  // manifest and the client that reads it have to agree about the bytes being hashed
  // without either of them having to serialise a struct.
  const std::string salt = ToString(version);

  std::vector<std::uint8_t> message;
  message.reserve(id.size() + salt.size());
  message.insert(message.end(), id.begin(), id.end());
  message.insert(message.end(), salt.begin(), salt.end());

  const auto digest = HashBytes(message);
  if (!digest.has_value()) {
    // No hash means no bucket, and the conservative bucket is the last one: an
    // installation that cannot compute where it stands does not join a partial rollout.
    return kFullRollout - 1;
  }

  std::uint64_t value = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>((*digest)[i]) << (8 * i);
  }
  // The modulo bias here is one part in 1.8e17, which is smaller than the difference
  // between 10% and 10.000000000000001% of every installation that will ever exist.
  return static_cast<int>(value % static_cast<std::uint64_t>(kFullRollout));
}

bool InRollout(const InstallId& id, const Version& version, int percent) {
  if (percent >= kFullRollout) {
    return true;
  }
  if (percent <= 0) {
    return false;
  }
  return RolloutBucket(id, version) < percent;
}

}  // namespace sonora::update
