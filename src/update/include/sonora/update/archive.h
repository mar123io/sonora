#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sonora/update/hash.h"

namespace sonora::update {

// The update archive: the payload's files, concatenated, uncompressed.
//
// ADR 0010 explains why it is uncompressed, and the short version is that the
// client has to be able to rebuild this file byte for byte from the files
// installed on its own disk, so that it can patch from it. Everything about the
// format therefore has to be decided by the tree's contents and by nothing else:
//
//   magic            8 bytes   "SONORAPK"
//   format version   u32       1
//   member count     u32
//   per member:      u16 path length
//                    path bytes, '/'-separated, relative
//                    u64 content size
//                    32  BLAKE2b-256 of the content
//                    u32 flags
//   then the members' contents, in the same order, back to back.
//
// No offsets (the reader walks forward once, with bounded memory), no
// timestamps, no permission bits beyond one, no compression, no padding and no
// alignment. Anything that two machines holding the same files could disagree
// about is a reason the delta path silently stops working, and a silent
// optimisation that stops working is indistinguishable from a broken one.
//
// Member order is the byte order of the paths, strictly increasing -- which also
// means no two members can name the same file, without a separate check.
//
// All integers are little-endian, written explicitly rather than by memcpy of a
// struct, because "the layout this compiler happens to choose" is exactly the
// kind of thing the previous paragraph is about.

inline constexpr std::string_view kArchiveMagic = "SONORAPK";
inline constexpr std::uint32_t kArchiveFormatVersion = 1;

// The only flag. It means nothing on Windows and it is here because an archive
// unpacked on macOS or Linux by the same code has to be able to produce an
// executable file, and losing the bit silently would produce an installation
// that looks complete and cannot start.
inline constexpr std::uint32_t kMemberExecutable = 1u << 0;

// Bounds, all of them arbitrary and all of them enforced. The payload has 242
// members and its largest is 137 MiB; these leave several orders of magnitude of
// room and still turn a corrupt length field into a refusal rather than an
// allocation.
inline constexpr std::uint32_t kMaxMembers = 65536;
inline constexpr std::uint16_t kMaxPathLength = 1024;
inline constexpr std::uint64_t kMaxMemberSize = 8ull * 1024 * 1024 * 1024;
inline constexpr std::uint64_t kMaxArchiveSize = 16ull * 1024 * 1024 * 1024;

struct Member {
  std::string path;
  std::uint64_t size = 0;
  Hash256 hash{};
  std::uint32_t flags = 0;
};

struct ArchiveHeader {
  std::vector<Member> members;
  // Where the first member's content starts, and how many bytes all of them
  // occupy. content_offset + content_size is the size of the whole archive.
  std::uint64_t content_offset = 0;
  std::uint64_t content_size = 0;
};

enum class ArchiveError {
  kNone,
  kTruncated,           // the bytes end in the middle of something
  kBadMagic,            // not one of these at all
  kUnsupportedVersion,  // a format from the future; refused, not guessed at
  kTooManyMembers,
  kMemberTooLarge,
  kArchiveTooLarge,
  kBadPath,       // see IsAcceptableMemberPath
  kNotSorted,     // paths not strictly increasing: unordered, or a duplicate
  kSizeMismatch,  // the declared contents are not the bytes that are there
  kUnknownFlags,  // a flag this version does not understand
};

[[nodiscard]] std::string_view Describe(ArchiveError error);

struct ParsedHeader {
  ArchiveError error = ArchiveError::kNone;
  ArchiveHeader header;

  [[nodiscard]] bool ok() const noexcept { return error == ArchiveError::kNone; }
};

// Reads the header out of the front of an archive.
//
// `bytes` may be a prefix of the file -- the header of a 213 MiB archive is about
// 20 KiB -- but `total_archive_size` must be the size of the whole thing, because
// checking that the declared contents add up to exactly the bytes present is half
// of what this function is for. Pass 0 to skip that check, which only the encoder's
// own round-trip test has any business doing.
//
// Total, never throws. Every path out of it is one of the values above.
[[nodiscard]] ParsedHeader ReadArchiveHeader(std::span<const std::uint8_t> bytes,
                                             std::uint64_t total_archive_size);

// The same rules the reader applies, available on their own because they are the
// interesting half and because the encoder applies them too: a builder that can
// produce an archive this reader refuses is a bug that only shows up on somebody
// else's machine.
//
// A path is acceptable if it is a non-empty, relative, '/'-separated sequence of
// segments where no segment is empty, ".", "..", a Windows reserved device name,
// or ends in a space or a dot; and no byte is a control character, a backslash, or
// one of : * ? " < > | -- the characters Windows will not put in a file name.
//
// The last two rules are the ones worth arguing for. This archive is written on a
// Linux runner and unpacked on Windows, so a name that is legal at the point of
// writing and illegal at the point of reading produces a release that cannot be
// installed, discovered by the person installing it. And "a." and "a" name the
// same file on Windows, which would turn two members into one.
[[nodiscard]] bool IsAcceptableMemberPath(std::string_view path);

// Serialises a header. Returns nothing if the members break any rule the reader
// enforces, including being out of order -- sorting them is the caller's job and
// its choice of comparison has to be the byte order of the paths, which is the
// one order that does not depend on a locale.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> EncodeArchiveHeader(
    std::span<const Member> members);

}  // namespace sonora::update
