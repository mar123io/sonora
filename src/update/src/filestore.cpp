#include "sonora/update/filestore.h"

#include <algorithm>
#include <fstream>
#include <system_error>
#include <utility>

namespace sonora::update {
namespace {

constexpr std::size_t kCopyChunk = 1 << 20;
constexpr std::uint64_t kMaxJournalBytes = 64 * 1024;
constexpr std::size_t kMaxHeaderBytes = 8 * 1024 * 1024;

// std::ifstream and std::ofstream take an fs::path directly (C++17), and on Windows
// that opens the wide path -- so a user whose profile directory has an accent in it
// works, and nothing in this file has to know which operating system it is on.
// Doing the same thing with fopen would have needed _wfopen and an #ifdef, which
// ADR 0002 does not allow outside src/platform/.
std::ifstream OpenForReading(const fs::path& path) {
  return std::ifstream(path, std::ios::binary);
}

std::ofstream OpenForWriting(const fs::path& path) {
  return std::ofstream(path, std::ios::binary | std::ios::trunc);
}

char* AsChars(std::uint8_t* bytes) { return reinterpret_cast<char*>(bytes); }
const char* AsChars(const std::uint8_t* bytes) { return reinterpret_cast<const char*>(bytes); }

// The relative path of `path` under `root`, with '/' separators, which is what an
// archive member is named by. generic_string() is the portable spelling: on Windows
// fs::path::string() hands back backslashes, and archive.h refuses those --
// correctly, because a name written on one machine has to be readable on another.
std::optional<std::string> MemberName(const fs::path& root, const fs::path& path) {
  std::error_code ec;
  const fs::path relative = fs::relative(path, root, ec);
  if (ec || relative.empty()) {
    return std::nullopt;
  }
  return relative.generic_string();
}

}  // namespace

std::string_view Describe(FileStoreError error) {
  switch (error) {
    case FileStoreError::kNone:
      return "no error";
    case FileStoreError::kNotADirectory:
      return "not a directory";
    case FileStoreError::kAlreadyExists:
      return "something is already there";
    case FileStoreError::kReadFailed:
      return "a file could not be read";
    case FileStoreError::kWriteFailed:
      return "a file could not be written";
    case FileStoreError::kBadPath:
      return "a name that an update package may not carry";
    case FileStoreError::kSymlink:
      return "a symbolic link, which a package may not contain";
    case FileStoreError::kNotAFile:
      return "something that is neither a file nor a directory";
    case FileStoreError::kTooLarge:
      return "larger than this version will handle";
    case FileStoreError::kArchiveInvalid:
      return "the package is not one this version can read";
    case FileStoreError::kHashMismatch:
      return "the bytes are not the bytes the package described";
  }
  return "unknown error";
}

const FlushFn& NoFlush() {
  // Deliberately not a durable write, and named so that nobody passes it by
  // accident. Closing the stream got the bytes out of this process; nothing here
  // gets them out of the operating system, so a power cut in the wrong microsecond
  // can lose a journal entry that was reported as written.
  static const FlushFn fn = [](const fs::path&) { return true; };
  return fn;
}

bool WriteFileDurably(const fs::path& path,
                      std::span<const std::uint8_t> bytes,
                      const FlushFn& flush,
                      FileStoreError& error) {
  error = FileStoreError::kNone;

  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);

  fs::path temporary = path;
  temporary += ".tmp";
  {
    std::ofstream out = OpenForWriting(temporary);
    if (!out) {
      error = FileStoreError::kWriteFailed;
      return false;
    }
    if (!bytes.empty()) {
      out.write(AsChars(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    out.close();
    if (!out) {
      error = FileStoreError::kWriteFailed;
      return false;
    }
  }
  if (!flush(temporary)) {
    error = FileStoreError::kWriteFailed;
    fs::remove(temporary, ec);
    return false;
  }

  // Rename over the old one. fs::rename replaces an existing file, which is the
  // whole reason for the temporary: the journal on the disk is either the old one or
  // the new one, and never a truncated one.
  fs::rename(temporary, path, ec);
  if (ec) {
    error = FileStoreError::kWriteFailed;
    fs::remove(temporary, ec);
    return false;
  }
  return true;
}

std::optional<std::vector<std::uint8_t>> ReadFile(const fs::path& path,
                                                  std::uint64_t max_bytes,
                                                  FileStoreError& error) {
  error = FileStoreError::kNone;

  std::error_code ec;
  if (fs::is_symlink(fs::symlink_status(path, ec))) {
    error = FileStoreError::kSymlink;
    return std::nullopt;
  }
  const std::uintmax_t size = fs::file_size(path, ec);
  if (ec) {
    error = FileStoreError::kReadFailed;
    return std::nullopt;
  }
  if (size > max_bytes) {
    error = FileStoreError::kTooLarge;
    return std::nullopt;
  }

  std::ifstream in = OpenForReading(path);
  if (!in) {
    error = FileStoreError::kReadFailed;
    return std::nullopt;
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  if (size > 0) {
    in.read(AsChars(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (in.gcount() != static_cast<std::streamsize>(bytes.size())) {
      error = FileStoreError::kReadFailed;
      return std::nullopt;
    }
  }
  return bytes;
}

std::optional<Hash256> HashFile(const fs::path& path, FileStoreError& error) {
  error = FileStoreError::kNone;
  std::ifstream in = OpenForReading(path);
  if (!in) {
    error = FileStoreError::kReadFailed;
    return std::nullopt;
  }
  Hasher hasher;
  if (!hasher.ok()) {
    error = FileStoreError::kReadFailed;
    return std::nullopt;
  }
  std::vector<std::uint8_t> buffer(kCopyChunk);
  while (in) {
    in.read(AsChars(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize read = in.gcount();
    if (read > 0) {
      hasher.Update(std::span(buffer).first(static_cast<std::size_t>(read)));
    }
  }
  if (in.bad()) {
    error = FileStoreError::kReadFailed;
    return std::nullopt;
  }
  return hasher.Finish();
}

bool ExecuteBitIsMeaningful(const fs::path& scratch_directory) {
  std::error_code ec;
  fs::create_directories(scratch_directory, ec);
  const fs::path probe = scratch_directory / ".sonora-permissions-probe";
  fs::remove(probe, ec);
  {
    std::ofstream out = OpenForWriting(probe);
    if (!out) {
      // Cannot ask, so do not answer. Recording no flags is the safe half of being wrong:
      // a file that should have been executable and is not can be fixed, and a payload
      // where everything is marked executable cannot be told from one where the marks
      // mean something.
      return false;
    }
  }
  const fs::file_status status = fs::status(probe, ec);
  const bool claims_executable =
      !ec && (status.permissions() & fs::perms::owner_exec) != fs::perms::none;
  fs::remove(probe, ec);
  // A file that was just created, is empty, and is already executable: the filesystem is
  // not reporting the bit, it is inventing one.
  return !claims_executable;
}

std::optional<ArchiveHeader> PackDirectory(const fs::path& dir,
                                           const fs::path& out,
                                           FileStoreError& error) {
  error = FileStoreError::kNone;

  std::error_code ec;
  if (!fs::is_directory(dir, ec)) {
    error = FileStoreError::kNotADirectory;
    return std::nullopt;
  }

  // Collect first, sort after: the order the filesystem hands entries back is not a
  // property of the tree, and the archive's order has to be.
  std::vector<std::pair<std::string, fs::path>> found;
  fs::recursive_directory_iterator walk(dir, fs::directory_options::none, ec);
  if (ec) {
    error = FileStoreError::kReadFailed;
    return std::nullopt;
  }
  for (const fs::directory_entry& entry : walk) {
    if (entry.is_symlink()) {
      error = FileStoreError::kSymlink;
      return std::nullopt;
    }
    if (entry.is_directory()) {
      continue;
    }
    if (!entry.is_regular_file()) {
      error = FileStoreError::kNotAFile;
      return std::nullopt;
    }
    auto name = MemberName(dir, entry.path());
    if (!name.has_value() || !IsAcceptableMemberPath(*name)) {
      error = FileStoreError::kBadPath;
      return std::nullopt;
    }
    found.emplace_back(std::move(*name), entry.path());
  }
  if (found.size() > kMaxMembers) {
    error = FileStoreError::kTooLarge;
    return std::nullopt;
  }
  std::sort(found.begin(), found.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });

  // Asked once, next to the archive being written rather than inside the tree being read:
  // packing must not modify what it is packing.
  const bool record_execute_bit = ExecuteBitIsMeaningful(out.parent_path());

  std::vector<Member> members;
  std::vector<fs::path> sources;
  members.reserve(found.size());
  sources.reserve(found.size());
  for (auto& [name, path] : found) {
    Member member;
    member.path = name;
    const std::uintmax_t size = fs::file_size(path, ec);
    if (ec) {
      error = FileStoreError::kReadFailed;
      return std::nullopt;
    }
    member.size = static_cast<std::uint64_t>(size);
    const auto hash = HashFile(path, error);
    if (!hash.has_value()) {
      return std::nullopt;
    }
    member.hash = *hash;
    // The executable bit, where the filesystem has one to report. On Windows it does
    // not, and it says so by reporting one on everything -- see ExecuteBitIsMeaningful.
    if (record_execute_bit) {
      const fs::file_status status = fs::status(path, ec);
      if (!ec && (status.permissions() & fs::perms::owner_exec) != fs::perms::none) {
        member.flags |= kMemberExecutable;
      }
    }
    members.push_back(std::move(member));
    sources.push_back(path);
  }

  const auto header = EncodeArchiveHeader(members);
  if (!header.has_value()) {
    error = FileStoreError::kBadPath;
    return std::nullopt;
  }

  std::ofstream output = OpenForWriting(out);
  if (!output) {
    error = FileStoreError::kWriteFailed;
    return std::nullopt;
  }
  output.write(AsChars(header->data()), static_cast<std::streamsize>(header->size()));

  std::vector<std::uint8_t> buffer(kCopyChunk);
  for (std::size_t i = 0; i < sources.size(); ++i) {
    std::ifstream source = OpenForReading(sources[i]);
    if (!source) {
      error = FileStoreError::kReadFailed;
      return std::nullopt;
    }
    std::uint64_t written = 0;
    while (written < members[i].size) {
      const auto want = static_cast<std::streamsize>(
          std::min<std::uint64_t>(buffer.size(), members[i].size - written));
      source.read(AsChars(buffer.data()), want);
      if (source.gcount() != want) {
        // The file shrank while it was being read. Not a corrupt archive: a tree
        // that changed under the packer, which would make the archive describe
        // something that never existed.
        error = FileStoreError::kReadFailed;
        return std::nullopt;
      }
      output.write(AsChars(buffer.data()), want);
      written += static_cast<std::uint64_t>(want);
    }
  }
  output.close();
  if (!output) {
    error = FileStoreError::kWriteFailed;
    return std::nullopt;
  }

  ArchiveHeader result;
  result.members = std::move(members);
  result.content_offset = header->size();
  result.content_size = 0;
  for (const Member& member : result.members) {
    result.content_size += member.size;
  }
  return result;
}

bool UnpackArchive(const fs::path& archive,
                   const fs::path& dir,
                   const FlushFn& flush,
                   FileStoreError& error) {
  error = FileStoreError::kNone;

  std::error_code ec;
  if (fs::exists(dir, ec)) {
    error = FileStoreError::kAlreadyExists;
    return false;
  }

  const std::uintmax_t total = fs::file_size(archive, ec);
  if (ec) {
    error = FileStoreError::kReadFailed;
    return false;
  }

  std::ifstream in = OpenForReading(archive);
  if (!in) {
    error = FileStoreError::kReadFailed;
    return false;
  }

  // The header first: 242 members is about 20 KiB, and anything that needs more
  // than this cap is not a Sonora package.
  std::vector<std::uint8_t> head(
      static_cast<std::size_t>(std::min<std::uintmax_t>(total, kMaxHeaderBytes)));
  in.read(AsChars(head.data()), static_cast<std::streamsize>(head.size()));
  head.resize(static_cast<std::size_t>(in.gcount()));
  in.clear();
  const ParsedHeader parsed = ReadArchiveHeader(head, static_cast<std::uint64_t>(total));
  if (!parsed.ok()) {
    error = FileStoreError::kArchiveInvalid;
    return false;
  }

  in.seekg(static_cast<std::streamoff>(parsed.header.content_offset), std::ios::beg);
  if (!in) {
    error = FileStoreError::kReadFailed;
    return false;
  }

  fs::create_directories(dir, ec);
  if (ec) {
    error = FileStoreError::kWriteFailed;
    return false;
  }

  std::vector<std::uint8_t> buffer(kCopyChunk);
  for (const Member& member : parsed.header.members) {
    // lexically_normal on a path the reader has already accepted: belt and braces,
    // and cheap. IsAcceptableMemberPath has refused every "..", every absolute path
    // and every backslash before this line runs.
    const fs::path target = dir / fs::path(member.path).lexically_normal();
    fs::create_directories(target.parent_path(), ec);
    std::ofstream output = OpenForWriting(target);
    if (!output) {
      error = FileStoreError::kWriteFailed;
      return false;
    }
    Hasher hasher;
    if (!hasher.ok()) {
      error = FileStoreError::kWriteFailed;
      return false;
    }
    std::uint64_t remaining = member.size;
    while (remaining > 0) {
      const auto want =
          static_cast<std::streamsize>(std::min<std::uint64_t>(buffer.size(), remaining));
      in.read(AsChars(buffer.data()), want);
      if (in.gcount() != want) {
        error = FileStoreError::kReadFailed;
        return false;
      }
      output.write(AsChars(buffer.data()), want);
      hasher.Update(std::span(buffer).first(static_cast<std::size_t>(want)));
      remaining -= static_cast<std::uint64_t>(want);
    }
    const auto hash = hasher.Finish();
    if (!hash.has_value() || !HashesEqual(*hash, member.hash)) {
      error = FileStoreError::kHashMismatch;
      return false;
    }
    output.close();
    if (!output || !flush(target)) {
      error = FileStoreError::kWriteFailed;
      return false;
    }
    if ((member.flags & kMemberExecutable) != 0) {
      fs::permissions(target, fs::perms::owner_exec | fs::perms::group_exec,
                      fs::perm_options::add, ec);
    }
  }
  return true;
}

bool RemoveTree(const fs::path& dir, FileStoreError& error) {
  error = FileStoreError::kNone;
  std::error_code ec;
  if (!fs::exists(dir, ec)) {
    return true;
  }
  fs::remove_all(dir, ec);
  if (ec) {
    error = FileStoreError::kWriteFailed;
    return false;
  }
  return true;
}

FileSystemRunner::FileSystemRunner(Layout layout, FlushFn flush)
    : layout_(std::move(layout)), flush_(std::move(flush)) {}

bool FileSystemRunner::WriteJournal(const Journal& journal) {
  const std::string text = EncodeJournal(journal);
  FileStoreError error = FileStoreError::kNone;
  const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t*>(text.data()),
                                            text.size());
  if (!WriteFileDurably(layout_.journal_file(), bytes, flush_, error)) {
    last_error_ = std::string(Describe(error));
    return false;
  }
  return true;
}

Facts FileSystemRunner::Look() {
  std::error_code ec;
  Facts facts;
  facts.current = fs::is_directory(layout_.current(), ec);
  facts.staged = fs::is_directory(layout_.staged(), ec);
  facts.previous = fs::is_directory(layout_.previous(), ec);
  facts.launch_ok = ReadLaunchFlag(layout_);
  return facts;
}

bool FileSystemRunner::Perform(Step step) {
  std::error_code ec;
  FileStoreError error = FileStoreError::kNone;
  const auto move = [&](const fs::path& from, const fs::path& to) {
    fs::rename(from, to, ec);
    if (ec) {
      last_error_ = ec.message();
      return false;
    }
    return true;
  };
  const auto remove = [&](const fs::path& what) {
    if (!RemoveTree(what, error)) {
      last_error_ = std::string(Describe(error));
      return false;
    }
    return true;
  };

  switch (step) {
    case Step::kNothing:
      return true;
    case Step::kMoveCurrentToPrevious:
      return move(layout_.current(), layout_.previous());
    case Step::kMoveStagedToCurrent:
      return move(layout_.staged(), layout_.current());
    case Step::kMovePreviousToCurrent:
      return move(layout_.previous(), layout_.current());
    case Step::kDiscardCurrent:
      return remove(layout_.current());
    case Step::kRemovePrevious:
      return remove(layout_.previous());
    case Step::kRemoveStaged:
      return remove(layout_.staged());
  }
  return false;
}

LoadedJournal LoadJournal(const Layout& layout) {
  LoadedJournal loaded;
  std::error_code ec;
  if (!fs::exists(layout.journal_file(), ec)) {
    return loaded;  // idle and readable: there has never been an update here
  }
  FileStoreError store_error = FileStoreError::kNone;
  const auto bytes = ReadFile(layout.journal_file(), kMaxJournalBytes, store_error);
  if (!bytes.has_value()) {
    loaded.readable = false;
    return loaded;
  }
  JournalError error = JournalError::kNone;
  const std::string_view text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
  const auto journal = ParseJournal(text, error);
  if (!journal.has_value()) {
    loaded.readable = false;
    return loaded;
  }
  loaded.journal = *journal;
  return loaded;
}

bool WriteLaunchFlag(const Layout& layout, const Version& version, const FlushFn& flush) {
  const std::string text = ToString(version);
  FileStoreError error = FileStoreError::kNone;
  const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t*>(text.data()),
                                            text.size());
  return WriteFileDurably(layout.launch_flag(), bytes, flush, error);
}

std::optional<Version> ReadLaunchFlag(const Layout& layout) {
  FileStoreError error = FileStoreError::kNone;
  const auto bytes = ReadFile(layout.launch_flag(), 64, error);
  if (!bytes.has_value()) {
    return std::nullopt;
  }
  const std::string_view text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
  return ParseVersion(text);
}

}  // namespace sonora::update
