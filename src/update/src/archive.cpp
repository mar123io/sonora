#include "sonora/update/archive.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>

namespace sonora::update {
namespace {

// A cursor over the bytes that cannot read past the end. Every read goes through
// it, so "did we check the length" is answered once instead of at fourteen call
// sites.
class Reader {
 public:
  explicit Reader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

  [[nodiscard]] bool Take(std::size_t count, std::span<const std::uint8_t>& out) {
    if (count > bytes_.size() - offset_) {
      return false;
    }
    out = bytes_.subspan(offset_, count);
    offset_ += count;
    return true;
  }

  [[nodiscard]] bool U16(std::uint16_t& out) {
    std::span<const std::uint8_t> raw;
    if (!Take(2, raw)) {
      return false;
    }
    out = static_cast<std::uint16_t>(static_cast<std::uint16_t>(raw[0]) |
                                     static_cast<std::uint16_t>(raw[1] << 8));
    return true;
  }

  [[nodiscard]] bool U32(std::uint32_t& out) {
    std::span<const std::uint8_t> raw;
    if (!Take(4, raw)) {
      return false;
    }
    out = 0;
    for (std::size_t i = 0; i < 4; ++i) {
      out |= static_cast<std::uint32_t>(raw[i]) << (8 * i);
    }
    return true;
  }

  [[nodiscard]] bool U64(std::uint64_t& out) {
    std::span<const std::uint8_t> raw;
    if (!Take(8, raw)) {
      return false;
    }
    out = 0;
    for (std::size_t i = 0; i < 8; ++i) {
      out |= static_cast<std::uint64_t>(raw[i]) << (8 * i);
    }
    return true;
  }

  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }

 private:
  std::span<const std::uint8_t> bytes_;
  std::size_t offset_ = 0;
};

void AppendU16(std::vector<std::uint8_t>& out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xFF));
  out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

void AppendU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF));
  }
}

void AppendU64(std::vector<std::uint8_t>& out, std::uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF));
  }
}

// CON, PRN, AUX, NUL, COM1..COM9, LPT1..LPT9 -- and their case variants, and
// with any extension: "con.txt" is still the console. This is the list Windows
// has carried since DOS and it is the reason an unpacker that trusts a path can
// fail in a way nobody would guess from reading the archive.
bool IsWindowsReservedName(std::string_view segment) {
  const std::size_t dot = segment.find('.');
  std::string_view stem = dot == std::string_view::npos ? segment : segment.substr(0, dot);
  if (stem.size() < 3 || stem.size() > 4) {
    return false;
  }
  std::string upper;
  upper.reserve(stem.size());
  for (const char c : stem) {
    upper.push_back(static_cast<char>(c >= 'a' && c <= 'z' ? c - ('a' - 'A') : c));
  }
  if (upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL") {
    return true;
  }
  if (upper.size() == 4 &&
      (upper.compare(0, 3, "COM") == 0 || upper.compare(0, 3, "LPT") == 0) && upper[3] >= '1' &&
      upper[3] <= '9') {
    return true;
  }
  return false;
}

bool IsAcceptableSegment(std::string_view segment) {
  if (segment.empty() || segment == "." || segment == "..") {
    return false;
  }
  if (segment.back() == '.' || segment.back() == ' ') {
    return false;
  }
  if (IsWindowsReservedName(segment)) {
    return false;
  }
  for (const char c : segment) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte < 0x20 || byte == 0x7F) {
      return false;
    }
    if (c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' ||
        c == '|') {
      return false;
    }
  }
  return true;
}

}  // namespace

std::string_view Describe(ArchiveError error) {
  switch (error) {
    case ArchiveError::kNone:
      return "no error";
    case ArchiveError::kTruncated:
      return "the archive ends in the middle of its header";
    case ArchiveError::kBadMagic:
      return "not a Sonora package";
    case ArchiveError::kUnsupportedVersion:
      return "a package format this version does not understand";
    case ArchiveError::kTooManyMembers:
      return "more members than a package may contain";
    case ArchiveError::kMemberTooLarge:
      return "a member larger than a package may contain";
    case ArchiveError::kArchiveTooLarge:
      return "a package larger than this version will read";
    case ArchiveError::kBadPath:
      return "a member path that cannot safely become a file name";
    case ArchiveError::kNotSorted:
      return "member paths out of order, or the same path twice";
    case ArchiveError::kSizeMismatch:
      return "the declared contents are not the bytes that are present";
    case ArchiveError::kUnknownFlags:
      return "a member flag this version does not understand";
  }
  return "unknown error";
}

bool IsAcceptableMemberPath(std::string_view path) {
  if (path.empty() || path.size() > kMaxPathLength) {
    return false;
  }
  if (path.front() == '/' || path.back() == '/') {
    return false;
  }
  std::size_t start = 0;
  while (start <= path.size()) {
    const std::size_t slash = path.find('/', start);
    const std::size_t end = slash == std::string_view::npos ? path.size() : slash;
    if (!IsAcceptableSegment(path.substr(start, end - start))) {
      return false;
    }
    if (slash == std::string_view::npos) {
      break;
    }
    start = slash + 1;
  }
  return true;
}

ParsedHeader ReadArchiveHeader(std::span<const std::uint8_t> bytes,
                               std::uint64_t total_archive_size) {
  ParsedHeader result;
  const auto fail = [&result](ArchiveError error) {
    result.error = error;
    result.header = {};
    return result;
  };

  Reader reader(bytes);

  std::span<const std::uint8_t> magic;
  if (!reader.Take(kArchiveMagic.size(), magic)) {
    return fail(ArchiveError::kTruncated);
  }
  if (std::memcmp(magic.data(), kArchiveMagic.data(), kArchiveMagic.size()) != 0) {
    return fail(ArchiveError::kBadMagic);
  }

  std::uint32_t format_version = 0;
  if (!reader.U32(format_version)) {
    return fail(ArchiveError::kTruncated);
  }
  if (format_version != kArchiveFormatVersion) {
    return fail(ArchiveError::kUnsupportedVersion);
  }

  std::uint32_t count = 0;
  if (!reader.U32(count)) {
    return fail(ArchiveError::kTruncated);
  }
  if (count > kMaxMembers) {
    return fail(ArchiveError::kTooManyMembers);
  }

  result.header.members.reserve(count);
  std::uint64_t content_size = 0;
  std::string previous_path;

  for (std::uint32_t i = 0; i < count; ++i) {
    std::uint16_t path_length = 0;
    if (!reader.U16(path_length)) {
      return fail(ArchiveError::kTruncated);
    }
    if (path_length == 0 || path_length > kMaxPathLength) {
      return fail(ArchiveError::kBadPath);
    }
    std::span<const std::uint8_t> path_bytes;
    if (!reader.Take(path_length, path_bytes)) {
      return fail(ArchiveError::kTruncated);
    }

    Member member;
    member.path.assign(reinterpret_cast<const char*>(path_bytes.data()), path_bytes.size());
    if (!IsAcceptableMemberPath(member.path)) {
      return fail(ArchiveError::kBadPath);
    }
    // Strictly increasing, which is both the order and the uniqueness check. The
    // comparison is over bytes; std::string's operator< is exactly that.
    if (i > 0 && !(previous_path < member.path)) {
      return fail(ArchiveError::kNotSorted);
    }
    previous_path = member.path;

    if (!reader.U64(member.size)) {
      return fail(ArchiveError::kTruncated);
    }
    if (member.size > kMaxMemberSize) {
      return fail(ArchiveError::kMemberTooLarge);
    }
    std::span<const std::uint8_t> hash_bytes;
    if (!reader.Take(member.hash.size(), hash_bytes)) {
      return fail(ArchiveError::kTruncated);
    }
    std::copy(hash_bytes.begin(), hash_bytes.end(), member.hash.begin());

    if (!reader.U32(member.flags)) {
      return fail(ArchiveError::kTruncated);
    }
    // An unknown flag is refused rather than masked off. A flag means something
    // about the file, and unpacking a file while ignoring what the writer said
    // about it is the behaviour that makes a format impossible to extend later.
    if ((member.flags & ~kMemberExecutable) != 0) {
      return fail(ArchiveError::kUnknownFlags);
    }

    // kMaxMemberSize * kMaxMembers overflows a u64, so the running total is
    // checked as it grows rather than at the end.
    if (content_size > kMaxArchiveSize - member.size) {
      return fail(ArchiveError::kArchiveTooLarge);
    }
    content_size += member.size;

    result.header.members.push_back(std::move(member));
  }

  result.header.content_offset = reader.offset();
  result.header.content_size = content_size;

  if (result.header.content_offset > kMaxArchiveSize - content_size) {
    return fail(ArchiveError::kArchiveTooLarge);
  }
  if (total_archive_size != 0 &&
      total_archive_size != result.header.content_offset + content_size) {
    return fail(ArchiveError::kSizeMismatch);
  }

  return result;
}

std::optional<std::vector<std::uint8_t>> EncodeArchiveHeader(std::span<const Member> members) {
  if (members.size() > kMaxMembers) {
    return std::nullopt;
  }

  std::vector<std::uint8_t> out;
  out.insert(out.end(), kArchiveMagic.begin(), kArchiveMagic.end());
  AppendU32(out, kArchiveFormatVersion);
  AppendU32(out, static_cast<std::uint32_t>(members.size()));

  std::uint64_t content_size = 0;
  for (std::size_t i = 0; i < members.size(); ++i) {
    const Member& member = members[i];
    if (!IsAcceptableMemberPath(member.path)) {
      return std::nullopt;
    }
    if (i > 0 && !(members[i - 1].path < member.path)) {
      return std::nullopt;
    }
    if (member.size > kMaxMemberSize) {
      return std::nullopt;
    }
    if ((member.flags & ~kMemberExecutable) != 0) {
      return std::nullopt;
    }
    if (content_size > kMaxArchiveSize - member.size) {
      return std::nullopt;
    }
    content_size += member.size;

    AppendU16(out, static_cast<std::uint16_t>(member.path.size()));
    out.insert(out.end(), member.path.begin(), member.path.end());
    AppendU64(out, member.size);
    out.insert(out.end(), member.hash.begin(), member.hash.end());
    AppendU32(out, member.flags);
  }

  if (out.size() > kMaxArchiveSize - content_size) {
    return std::nullopt;
  }
  return out;
}

}  // namespace sonora::update
