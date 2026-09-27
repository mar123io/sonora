#include "sonora/update/patch.h"

#include <zstd.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <new>

namespace sonora::update {
namespace {

// The largest archive this will produce, matching archive.h's own cap. A patch
// arrives from the network and its declared output size is the one number that
// decides how much memory this function asks for.
constexpr std::uint64_t kMaxOutputSize = 16ull * 1024 * 1024 * 1024;

struct CCtxDeleter {
  void operator()(ZSTD_CCtx* ctx) const noexcept { ZSTD_freeCCtx(ctx); }
};
struct DCtxDeleter {
  void operator()(ZSTD_DCtx* ctx) const noexcept { ZSTD_freeDCtx(ctx); }
};

}  // namespace

int PatchWindowLogFor(std::uint64_t old_size) {
  int log = kMinPatchWindowLog;
  while (log < kMaxPatchWindowLog && (1ull << log) < old_size) {
    ++log;
  }
  return log;
}

std::string_view Describe(PatchError error) {
  switch (error) {
    case PatchError::kNone:
      return "no error";
    case PatchError::kUnknownSize:
      return "the patch does not say how large its result is";
    case PatchError::kSizeMismatch:
      return "the patch produces a different size from the one the manifest names";
    case PatchError::kTooLarge:
      return "the patch produces more than this version will hold";
    case PatchError::kCorrupt:
      return "the patch is not a valid patch for this version";
    case PatchError::kOutOfMemory:
      return "not enough memory to apply the patch";
    case PatchError::kOverlappingInputs:
      return "the old and new payloads are the same bytes in memory";
  }
  return "unknown error";
}

namespace {

// std::less over pointers rather than < : comparing pointers into unrelated
// objects with the built-in operator is unspecified, and this function's whole
// job is to compare pointers into what are probably unrelated objects.
bool Overlaps(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b) {
  if (a.empty() || b.empty()) {
    return false;
  }
  const std::less<const std::uint8_t*> before;
  return !before(a.data() + a.size() - 1, b.data()) &&
         !before(b.data() + b.size() - 1, a.data());
}

}  // namespace

std::optional<std::vector<std::uint8_t>> ApplyPatch(std::span<const std::uint8_t> old_bytes,
                                                    std::span<const std::uint8_t> patch,
                                                    std::uint64_t expected_size,
                                                    PatchError& error) {
  error = PatchError::kNone;

  if (expected_size == 0 || expected_size > kMaxOutputSize) {
    error = PatchError::kTooLarge;
    return std::nullopt;
  }

  // What the frame claims, before anything is allocated. ZSTD_CONTENTSIZE_UNKNOWN
  // means the producer did not record it, which our generator always does, so a
  // patch without it did not come from our generator.
  const unsigned long long declared = ZSTD_getFrameContentSize(patch.data(), patch.size());
  if (declared == ZSTD_CONTENTSIZE_ERROR) {
    error = PatchError::kCorrupt;
    return std::nullopt;
  }
  if (declared == ZSTD_CONTENTSIZE_UNKNOWN) {
    error = PatchError::kUnknownSize;
    return std::nullopt;
  }
  if (declared != expected_size) {
    error = PatchError::kSizeMismatch;
    return std::nullopt;
  }

  const std::unique_ptr<ZSTD_DCtx, DCtxDeleter> dctx(ZSTD_createDCtx());
  if (dctx == nullptr) {
    error = PatchError::kOutOfMemory;
    return std::nullopt;
  }

  // The default window limit is smaller than the window a patch over a 213 MiB
  // prefix needs, and the failure it produces is a plain "corrupt frame" -- so
  // this line is the difference between a working updater and a mystery.
  if (ZSTD_isError(ZSTD_DCtx_setParameter(dctx.get(), ZSTD_d_windowLogMax, 31))) {
    error = PatchError::kCorrupt;
    return std::nullopt;
  }

  std::vector<std::uint8_t> out;
  try {
    out.resize(static_cast<std::size_t>(expected_size));
  } catch (const std::bad_alloc&) {
    error = PatchError::kOutOfMemory;
    return std::nullopt;
  }

  // refPrefix rather than a dictionary, and the streaming entry point rather than
  // the one-shot: the sticky parameters set on the context -- the window limit
  // above, the prefix here -- are honoured by ZSTD_decompressStream, and the
  // simpler-looking ZSTD_decompress_usingDict is a different path that does not
  // need to be.
  const std::uint8_t empty = 0;
  if (ZSTD_isError(ZSTD_DCtx_refPrefix(
          dctx.get(), old_bytes.empty() ? &empty : old_bytes.data(), old_bytes.size()))) {
    error = PatchError::kCorrupt;
    return std::nullopt;
  }

  ZSTD_inBuffer in{patch.data(), patch.size(), 0};
  ZSTD_outBuffer output{out.data(), out.size(), 0};
  const std::size_t remaining = ZSTD_decompressStream(dctx.get(), &output, &in);
  if (ZSTD_isError(remaining)) {
    error = PatchError::kCorrupt;
    return std::nullopt;
  }
  // Three separate things have to be true, and only the first is what zstd
  // reports: the frame ended, it produced exactly the promised bytes, and the
  // whole patch was consumed. Trailing bytes after a complete frame are a patch
  // that says more than it did, and that is a refusal rather than a shrug.
  if (remaining != 0 || output.pos != out.size() || in.pos != in.size) {
    error = PatchError::kCorrupt;
    return std::nullopt;
  }

  return out;
}

std::optional<std::uint64_t> DeclaredFrameSize(std::span<const std::uint8_t> frame) {
  const unsigned long long declared = ZSTD_getFrameContentSize(frame.data(), frame.size());
  if (declared == ZSTD_CONTENTSIZE_ERROR || declared == ZSTD_CONTENTSIZE_UNKNOWN) {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(declared);
}

std::optional<std::vector<std::uint8_t>> DecompressFrame(std::span<const std::uint8_t> frame,
                                                         std::uint64_t expected_size,
                                                         PatchError& error) {
  error = PatchError::kNone;

  if (expected_size == 0 || expected_size > kMaxOutputSize) {
    error = PatchError::kTooLarge;
    return std::nullopt;
  }

  const unsigned long long declared = ZSTD_getFrameContentSize(frame.data(), frame.size());
  if (declared == ZSTD_CONTENTSIZE_ERROR) {
    error = PatchError::kCorrupt;
    return std::nullopt;
  }
  if (declared == ZSTD_CONTENTSIZE_UNKNOWN) {
    error = PatchError::kUnknownSize;
    return std::nullopt;
  }
  if (declared != expected_size) {
    error = PatchError::kSizeMismatch;
    return std::nullopt;
  }

  const std::unique_ptr<ZSTD_DCtx, DCtxDeleter> dctx(ZSTD_createDCtx());
  if (dctx == nullptr) {
    error = PatchError::kOutOfMemory;
    return std::nullopt;
  }
  if (ZSTD_isError(ZSTD_DCtx_setParameter(dctx.get(), ZSTD_d_windowLogMax, 31))) {
    error = PatchError::kCorrupt;
    return std::nullopt;
  }

  std::vector<std::uint8_t> out;
  try {
    out.resize(static_cast<std::size_t>(expected_size));
  } catch (const std::bad_alloc&) {
    error = PatchError::kOutOfMemory;
    return std::nullopt;
  }

  ZSTD_inBuffer in{frame.data(), frame.size(), 0};
  ZSTD_outBuffer output{out.data(), out.size(), 0};
  const std::size_t remaining = ZSTD_decompressStream(dctx.get(), &output, &in);
  if (ZSTD_isError(remaining) || remaining != 0 || output.pos != out.size() ||
      in.pos != in.size) {
    error = PatchError::kCorrupt;
    return std::nullopt;
  }
  return out;
}

std::optional<std::vector<std::uint8_t>> CompressFrame(std::span<const std::uint8_t> bytes,
                                                       PatchError& error,
                                                       int level) {
  error = PatchError::kNone;

  const std::unique_ptr<ZSTD_CCtx, CCtxDeleter> cctx(ZSTD_createCCtx());
  if (cctx == nullptr) {
    error = PatchError::kOutOfMemory;
    return std::nullopt;
  }
  const auto set = [&cctx](ZSTD_cParameter parameter, int value) {
    return !ZSTD_isError(ZSTD_CCtx_setParameter(cctx.get(), parameter, value));
  };
  // The content size in the header is what DecompressFrame refuses to work without.
  // Multithreading is allowed here and not above: the output of this is not a patch,
  // it is a download, and what matters about it is its size on the wire.
  if (!set(ZSTD_c_compressionLevel, level) || !set(ZSTD_c_contentSizeFlag, 1)) {
    error = PatchError::kCorrupt;
    return std::nullopt;
  }

  std::vector<std::uint8_t> out;
  try {
    out.resize(ZSTD_compressBound(bytes.size()));
  } catch (const std::bad_alloc&) {
    error = PatchError::kOutOfMemory;
    return std::nullopt;
  }
  const std::uint8_t empty = 0;
  const std::size_t rc = ZSTD_compress2(cctx.get(), out.data(), out.size(),
                                        bytes.empty() ? &empty : bytes.data(), bytes.size());
  if (ZSTD_isError(rc)) {
    error = PatchError::kCorrupt;
    return std::nullopt;
  }
  out.resize(rc);
  return out;
}

std::optional<std::vector<std::uint8_t>> CreatePatch(std::span<const std::uint8_t> old_bytes,
                                                     std::span<const std::uint8_t> new_bytes,
                                                     PatchError& error,
                                                     int level) {
  error = PatchError::kNone;

  if (Overlaps(old_bytes, new_bytes)) {
    error = PatchError::kOverlappingInputs;
    return std::nullopt;
  }

  const std::unique_ptr<ZSTD_CCtx, CCtxDeleter> cctx(ZSTD_createCCtx());
  if (cctx == nullptr) {
    error = PatchError::kOutOfMemory;
    return std::nullopt;
  }

  const auto set = [&cctx](ZSTD_cParameter parameter, int value) {
    return !ZSTD_isError(ZSTD_CCtx_setParameter(cctx.get(), parameter, value));
  };

  if (!set(ZSTD_c_compressionLevel, level) ||
      // See kPatchWorkers: this is the setting that halves the patch.
      !set(ZSTD_c_nbWorkers, kPatchWorkers) || !set(ZSTD_c_enableLongDistanceMatching, 1) ||
      !set(ZSTD_c_windowLog, PatchWindowLogFor(old_bytes.size())) ||
      // The content size in the frame header is what ApplyPatch refuses to work
      // without, so it is not optional here either.
      !set(ZSTD_c_contentSizeFlag, 1)) {
    error = PatchError::kCorrupt;
    return std::nullopt;
  }

  std::vector<std::uint8_t> out;
  try {
    out.resize(ZSTD_compressBound(new_bytes.size()));
  } catch (const std::bad_alloc&) {
    error = PatchError::kOutOfMemory;
    return std::nullopt;
  }

  const std::uint8_t empty = 0;
  if (ZSTD_isError(ZSTD_CCtx_refPrefix(
          cctx.get(), old_bytes.empty() ? &empty : old_bytes.data(), old_bytes.size()))) {
    error = PatchError::kCorrupt;
    return std::nullopt;
  }

  const std::size_t rc =
      ZSTD_compress2(cctx.get(), out.data(), out.size(),
                     new_bytes.empty() ? &empty : new_bytes.data(), new_bytes.size());
  if (ZSTD_isError(rc)) {
    error = PatchError::kCorrupt;
    return std::nullopt;
  }
  out.resize(rc);
  return out;
}

}  // namespace sonora::update
