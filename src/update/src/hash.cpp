#include "sonora/update/hash.h"

#include <sodium.h>

#include <cstring>

namespace sonora::update {
namespace {

int HexValue(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  return -1;
}

}  // namespace

bool EnsureCryptoReady() {
  // sodium_init returns 0 on the first successful call and 1 if the library was
  // already initialised; both are success. Only a negative value is a failure.
  static const bool ready = sodium_init() >= 0;
  return ready;
}

struct Hasher::State {
  crypto_generichash_state state{};
  bool finished = false;
};

Hasher::Hasher() {
  if (!EnsureCryptoReady()) {
    return;
  }
  auto state = std::make_unique<State>();
  if (crypto_generichash_init(&state->state, nullptr, 0, sizeof(Hash256)) != 0) {
    return;
  }
  state_ = std::move(state);
}

Hasher::~Hasher() = default;
Hasher::Hasher(Hasher&&) noexcept = default;
Hasher& Hasher::operator=(Hasher&&) noexcept = default;

void Hasher::Update(std::span<const std::uint8_t> bytes) {
  if (state_ == nullptr || state_->finished || bytes.empty()) {
    return;
  }
  crypto_generichash_update(&state_->state, bytes.data(), bytes.size());
}

std::optional<Hash256> Hasher::Finish() {
  if (state_ == nullptr || state_->finished) {
    return std::nullopt;
  }
  state_->finished = true;
  Hash256 out{};
  if (crypto_generichash_final(&state_->state, out.data(), out.size()) != 0) {
    return std::nullopt;
  }
  return out;
}

std::optional<Hash256> HashBytes(std::span<const std::uint8_t> bytes) {
  if (!EnsureCryptoReady()) {
    return std::nullopt;
  }
  Hash256 out{};
  // An empty span has a null data pointer, and libsodium is documented to accept
  // a zero length but not to promise anything about a null pointer with it.
  const std::uint8_t empty = 0;
  const std::uint8_t* const data = bytes.empty() ? &empty : bytes.data();
  if (crypto_generichash(out.data(), out.size(), data, bytes.size(), nullptr, 0) != 0) {
    return std::nullopt;
  }
  return out;
}

std::string ToHex(std::span<const std::uint8_t> bytes) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  out.resize(bytes.size() * 2);
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    out[i * 2] = kDigits[bytes[i] >> 4];
    out[i * 2 + 1] = kDigits[bytes[i] & 0x0F];
  }
  return out;
}

std::optional<Hash256> ParseHash(std::string_view hex) {
  if (hex.size() != sizeof(Hash256) * 2) {
    return std::nullopt;
  }
  Hash256 out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    const int hi = HexValue(hex[i * 2]);
    const int lo = HexValue(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) {
      return std::nullopt;
    }
    out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
  }
  return out;
}

bool HashesEqual(const Hash256& a, const Hash256& b) {
  if (!EnsureCryptoReady()) {
    return false;
  }
  return sodium_memcmp(a.data(), b.data(), a.size()) == 0;
}

}  // namespace sonora::update
