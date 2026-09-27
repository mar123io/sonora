#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "sonora/update/archive.h"
#include "sonora/update/hash.h"

namespace {

using sonora::update::ArchiveError;
using sonora::update::EncodeArchiveHeader;
using sonora::update::Hash256;
using sonora::update::IsAcceptableMemberPath;
using sonora::update::kMemberExecutable;
using sonora::update::Member;
using sonora::update::ReadArchiveHeader;

Member Make(std::string path, std::uint64_t size, std::uint32_t flags = 0) {
  Member member;
  member.path = std::move(path);
  member.size = size;
  member.flags = flags;
  // The contents are not part of the header, so a plausible hash is enough here;
  // the round-trip of real contents is the patch test's business.
  member.hash.fill(static_cast<std::uint8_t>(size & 0xFF));
  return member;
}

// A whole archive: the encoded header followed by `size` bytes per member, so that
// the size check in ReadArchiveHeader has something true to check against.
std::vector<std::uint8_t> Whole(std::span<const Member> members) {
  const auto header = EncodeArchiveHeader(members);
  REQUIRE(header.has_value());
  std::vector<std::uint8_t> bytes = *header;
  for (const Member& member : members) {
    bytes.insert(bytes.end(), static_cast<std::size_t>(member.size), 0x5A);
  }
  return bytes;
}

}  // namespace

TEST_CASE("an archive round-trips through its own header") {
  const std::vector<Member> members = {Make("Sonora.exe", 4, kMemberExecutable),
                                       Make("libcef.dll", 7), Make("locales/it.pak", 2)};
  const std::vector<std::uint8_t> bytes = Whole(members);

  const auto parsed = ReadArchiveHeader(bytes, bytes.size());
  REQUIRE(parsed.ok());
  REQUIRE(parsed.header.members.size() == 3);
  CHECK(parsed.header.members[0].path == "Sonora.exe");
  CHECK(parsed.header.members[0].flags == kMemberExecutable);
  CHECK(parsed.header.members[1].path == "libcef.dll");
  CHECK(parsed.header.members[1].size == 7);
  CHECK(parsed.header.members[2].path == "locales/it.pak");
  CHECK(parsed.header.content_size == 13);
  CHECK(parsed.header.content_offset + parsed.header.content_size == bytes.size());
}

TEST_CASE("an archive with no members is still an archive") {
  const std::vector<Member> none;
  const std::vector<std::uint8_t> bytes = Whole(none);
  const auto parsed = ReadArchiveHeader(bytes, bytes.size());
  REQUIRE(parsed.ok());
  CHECK(parsed.header.members.empty());
  CHECK(parsed.header.content_size == 0);
}

TEST_CASE("something that is not one of these is not read as one") {
  const std::string other = "PK\x03\x04 and then some";
  const std::vector<std::uint8_t> bytes(other.begin(), other.end());
  CHECK(ReadArchiveHeader(bytes, bytes.size()).error == ArchiveError::kBadMagic);
  CHECK(ReadArchiveHeader({}, 0).error == ArchiveError::kTruncated);
}

TEST_CASE("a format version from the future is refused rather than guessed at") {
  const std::vector<Member> members = {Make("a", 1)};
  std::vector<std::uint8_t> bytes = Whole(members);
  bytes[8] = 2;  // the format version, little-endian, right after the magic
  CHECK(ReadArchiveHeader(bytes, bytes.size()).error == ArchiveError::kUnsupportedVersion);
}

TEST_CASE("truncation anywhere in the header is truncation, not a partial archive") {
  const std::vector<Member> members = {Make("Sonora.exe", 3), Make("libcef.dll", 5)};
  const std::vector<std::uint8_t> bytes = Whole(members);
  const auto full = ReadArchiveHeader(bytes, bytes.size());
  REQUIRE(full.ok());

  // Every prefix of the header, one byte at a time. Not a sample: the check is
  // that there is no length at which this reader walks off the end, and only
  // every length answers that.
  for (std::size_t length = 0; length < full.header.content_offset; ++length) {
    const auto parsed = ReadArchiveHeader(std::span(bytes).first(length), 0);
    CHECK_FALSE(parsed.ok());
  }
}

TEST_CASE("the declared contents have to be the bytes that are there") {
  const std::vector<Member> members = {Make("a", 10)};
  const std::vector<std::uint8_t> bytes = Whole(members);
  CHECK(ReadArchiveHeader(bytes, bytes.size()).ok());
  CHECK(ReadArchiveHeader(bytes, bytes.size() - 1).error == ArchiveError::kSizeMismatch);
  CHECK(ReadArchiveHeader(bytes, bytes.size() + 1).error == ArchiveError::kSizeMismatch);
}

TEST_CASE("members out of order, or twice, are refused") {
  std::vector<Member> members = {Make("b", 1), Make("a", 1)};
  CHECK_FALSE(EncodeArchiveHeader(members).has_value());

  members = {Make("a", 1), Make("a", 1)};
  CHECK_FALSE(EncodeArchiveHeader(members).has_value());

  // ...and the reader refuses what the encoder would not write, because the
  // archive it reads was not necessarily written by the encoder.
  std::vector<Member> sorted = {Make("a", 1), Make("b", 1)};
  std::vector<std::uint8_t> bytes = Whole(sorted);
  // Swap the two one-character paths in place: same length, so the header stays
  // the shape it was.
  const auto at_a = std::find(bytes.begin(), bytes.end(), static_cast<std::uint8_t>('a'));
  const auto at_b = std::find(bytes.begin(), bytes.end(), static_cast<std::uint8_t>('b'));
  REQUIRE(at_a != bytes.end());
  REQUIRE(at_b != bytes.end());
  std::iter_swap(at_a, at_b);
  CHECK(ReadArchiveHeader(bytes, bytes.size()).error == ArchiveError::kNotSorted);
}

TEST_CASE("an unknown flag is refused, not masked off") {
  const std::vector<Member> members = {Make("a", 1, kMemberExecutable << 1)};
  CHECK_FALSE(EncodeArchiveHeader(members).has_value());

  // ...and by the reader, which is the half that matters: the encoder only ever
  // sees flags this version wrote, and the reader sees whatever arrived. A mutation
  // that made the reader mask unknown flags off survived the first version of this
  // file, because only the encoder was being tested.
  const std::vector<Member> plain = {Make("a", 1)};
  std::vector<std::uint8_t> bytes = Whole(plain);
  const auto parsed = ReadArchiveHeader(bytes, bytes.size());
  REQUIRE(parsed.ok());
  // The flags are the last four bytes of the header, little-endian.
  bytes[parsed.header.content_offset - 4] = 0x02;
  CHECK(ReadArchiveHeader(bytes, bytes.size()).error == ArchiveError::kUnknownFlags);

  // The one flag this version does know is read back as itself.
  bytes[parsed.header.content_offset - 4] = 0x01;
  const auto with_flag = ReadArchiveHeader(bytes, bytes.size());
  REQUIRE(with_flag.ok());
  CHECK(with_flag.header.members.front().flags == kMemberExecutable);
}

TEST_CASE("the paths an archive may name") {
  CHECK(IsAcceptableMemberPath("Sonora.exe"));
  CHECK(IsAcceptableMemberPath("locales/it.pak"));
  CHECK(IsAcceptableMemberPath("swiftshader/libvk_swiftshader.dll"));
  CHECK(IsAcceptableMemberPath("a/b/c/d/e/f.txt"));
  CHECK(IsAcceptableMemberPath("file with spaces.dll"));
  CHECK(IsAcceptableMemberPath("accentate-àèìòù.pak"));
}

TEST_CASE("the paths it may not") {
  // Escaping the installation directory, in the four spellings that work.
  CHECK_FALSE(IsAcceptableMemberPath("../Sonora.exe"));
  CHECK_FALSE(IsAcceptableMemberPath("a/../../b"));
  CHECK_FALSE(IsAcceptableMemberPath("/etc/passwd"));
  CHECK_FALSE(IsAcceptableMemberPath("C:/Windows/System32/kernel32.dll"));
  CHECK_FALSE(IsAcceptableMemberPath("..\\Sonora.exe"));

  // Separators that are separators on the machine that unpacks and not on the one
  // that packs. This is why the backslash is refused rather than translated.
  CHECK_FALSE(IsAcceptableMemberPath("locales\\it.pak"));

  CHECK_FALSE(IsAcceptableMemberPath(""));
  CHECK_FALSE(IsAcceptableMemberPath("."));
  CHECK_FALSE(IsAcceptableMemberPath(".."));
  CHECK_FALSE(IsAcceptableMemberPath("a//b"));
  CHECK_FALSE(IsAcceptableMemberPath("a/"));
  CHECK_FALSE(IsAcceptableMemberPath("a/./b"));
  CHECK_FALSE(IsAcceptableMemberPath(std::string(2000, 'a')));

  // Characters Windows will not put in a file name, so an archive naming them is
  // a release that cannot be installed.
  CHECK_FALSE(IsAcceptableMemberPath("a*b"));
  CHECK_FALSE(IsAcceptableMemberPath("a?b"));
  CHECK_FALSE(IsAcceptableMemberPath("a\"b"));
  CHECK_FALSE(IsAcceptableMemberPath("a<b"));
  CHECK_FALSE(IsAcceptableMemberPath("a|b"));
  CHECK_FALSE(IsAcceptableMemberPath(std::string("a\0b", 3)));
  CHECK_FALSE(IsAcceptableMemberPath("a\nb"));
  CHECK_FALSE(
      IsAcceptableMemberPath("a\x7F"
                             "b"));

  // Names that Windows resolves to a device rather than a file, with or without
  // an extension, in any case.
  CHECK_FALSE(IsAcceptableMemberPath("CON"));
  CHECK_FALSE(IsAcceptableMemberPath("con.txt"));
  CHECK_FALSE(IsAcceptableMemberPath("locales/NUL.pak"));
  CHECK_FALSE(IsAcceptableMemberPath("Com1"));
  CHECK_FALSE(IsAcceptableMemberPath("lpt9.dll"));
  // ...but only those names. COM0 and COM10 are files.
  CHECK(IsAcceptableMemberPath("COM0"));
  CHECK(IsAcceptableMemberPath("COM10"));
  CHECK(IsAcceptableMemberPath("CONSOLE.dll"));
  CHECK(IsAcceptableMemberPath("auxiliary.pak"));

  // A trailing dot or space: Windows strips them, so "a." and "a" would be the
  // same file and two members would become one.
  CHECK_FALSE(IsAcceptableMemberPath("a."));
  CHECK_FALSE(IsAcceptableMemberPath("a "));
  CHECK_FALSE(IsAcceptableMemberPath("dir./a"));
}

TEST_CASE("the encoder refuses every path the reader would refuse") {
  for (const std::string_view path : {"../a", "/a", "a\\b", "CON", "a.", "a/", "", "a//b"}) {
    const std::vector<Member> members = {Make(std::string(path), 1)};
    CHECK_FALSE(EncodeArchiveHeader(members).has_value());
  }
}

TEST_CASE("a hash has one spelling") {
  const Hash256 hash{};
  CHECK(sonora::update::ToHex(hash) == std::string(64, '0'));
  CHECK(sonora::update::ParseHash(std::string(64, '0')).has_value());
  CHECK_FALSE(sonora::update::ParseHash(std::string(64, '0') + "0").has_value());
  CHECK_FALSE(sonora::update::ParseHash(std::string(63, '0')).has_value());
  CHECK_FALSE(sonora::update::ParseHash("0x" + std::string(62, '0')).has_value());
  CHECK_FALSE(sonora::update::ParseHash(std::string(64, 'A')).has_value());
}

TEST_CASE("hashing the same bytes twice gives the same hash, and different bytes do not") {
  const std::vector<std::uint8_t> a = {1, 2, 3};
  const std::vector<std::uint8_t> b = {1, 2, 4};
  const auto ha = sonora::update::HashBytes(a);
  const auto hb = sonora::update::HashBytes(b);
  REQUIRE(ha.has_value());
  REQUIRE(hb.has_value());
  CHECK(sonora::update::HashesEqual(*ha, *sonora::update::HashBytes(a)));
  CHECK_FALSE(sonora::update::HashesEqual(*ha, *hb));

  // ...and the incremental hasher agrees with the one-shot, whatever the split.
  for (std::size_t split = 0; split <= a.size(); ++split) {
    sonora::update::Hasher hasher;
    REQUIRE(hasher.ok());
    hasher.Update(std::span(a).first(split));
    hasher.Update(std::span(a).subspan(split));
    const auto incremental = hasher.Finish();
    REQUIRE(incremental.has_value());
    CHECK(sonora::update::HashesEqual(*ha, *incremental));
    CHECK_FALSE(hasher.Finish().has_value());
  }
}

TEST_CASE("the hash is BLAKE2b-256, and another implementation agrees about it") {
  // Golden vectors from Python's hashlib.blake2b(digest_size=32), which is what
  // tools/gen_manifest.py uses. The manifest is written by that script and read by
  // this code, so the two have to agree about what a hash is -- and "they agree
  // because both call libsodium" is not true here: one of them does not.
  //
  // Three known answers cost nothing and catch the mistake this arrangement invites,
  // which is one side hashing with SHA-256 or with BLAKE2b's default 64-byte digest
  // and the release simply never being installable.
  struct Vector {
    std::string_view input;
    std::string_view hex;
  };
  for (const Vector vector :
       {Vector{"", "0e5751c026e543b2e8ab2eb06099daa1d1e5df47778f7787faab45cdf12fe3a8"},
        Vector{"abc", "bddd813c634239723171ef3fee98579b94964e3bb1cb3e427262c8c068d52319"},
        Vector{"sonora", "46c1d1cd5dbaa432d72edd77422f014091bff9bfc7296b8e44a1ac6bb8f637d2"}}) {
    const std::span<const std::uint8_t> bytes(
        reinterpret_cast<const std::uint8_t*>(vector.input.data()), vector.input.size());
    const auto hash = sonora::update::HashBytes(bytes);
    REQUIRE(hash.has_value());
    CHECK(sonora::update::ToHex(*hash) == vector.hex);
  }
}

TEST_CASE("an empty input has a hash, and it is not zero") {
  const auto empty = sonora::update::HashBytes({});
  REQUIRE(empty.has_value());
  CHECK_FALSE(sonora::update::HashesEqual(*empty, Hash256{}));
}
