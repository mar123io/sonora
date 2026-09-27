#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sonora/update/archive.h"
#include "sonora/update/driver.h"
#include "sonora/update/hash.h"
#include "sonora/update/journal.h"

namespace sonora::update {

// The archive and the journal, against a real filesystem.
//
// std::filesystem, not a platform header, and that is not a shortcut: walking a
// directory, renaming one and creating a file are the same operations everywhere,
// and ADR 0002's rule is about the things that are not. What is left over -- making
// a write survive a power cut, copying the running executable somewhere else,
// starting a process and waiting for one -- is in src/platform/, and it is four
// functions.
//
// The exception is the flush, which is why WriteFileDurably takes one. See below.

namespace fs = std::filesystem;

// Where the four directories are. ADR 0011 requires them to be siblings so that
// every move is a rename inside one directory.
struct Layout {
  fs::path root;
  std::string name = "Sonora";

  [[nodiscard]] fs::path current() const { return root / name; }
  [[nodiscard]] fs::path staged() const { return root / (name + ".new"); }
  [[nodiscard]] fs::path previous() const { return root / (name + ".old"); }
  [[nodiscard]] fs::path work() const { return root / (name + ".update"); }
  [[nodiscard]] fs::path journal_file() const { return work() / "journal"; }
  [[nodiscard]] fs::path launch_flag() const { return work() / "launch-ok"; }
  [[nodiscard]] fs::path download_dir() const { return work() / "download"; }
};

enum class FileStoreError {
  kNone,
  kNotADirectory,
  kAlreadyExists,
  kReadFailed,
  kWriteFailed,
  kBadPath,   // a name in the tree that an archive may not carry -- see archive.h
  kSymlink,   // refused rather than followed or copied
  kNotAFile,  // a device, a socket, a fifo: not something a payload contains
  kTooLarge,
  kArchiveInvalid,
  kHashMismatch,
};

[[nodiscard]] std::string_view Describe(FileStoreError error);

// Making a write survive a power cut is the one thing here that no portable API
// does: closing a stream moves bytes out of the process, and nothing in the
// standard moves them out of the operating system's cache. So the caller passes
// that in, as a function of the path of a file that has just been closed.
//
// A path rather than a handle, and that is what keeps this file free of any
// knowledge of which operating system it is on: FlushFileBuffers wants a HANDLE and
// fsync wants a descriptor, and neither of those two words can appear here (ADR
// 0002). src/platform/win/update_host_win32.cpp supplies a real one in four lines.
//
// NoFlush() is the honest default -- correct in every respect except the one that
// only a power cut can tell -- and it is what the tests and the CI end-to-end job
// use, because a test cannot tell either.
using FlushFn = std::function<bool(const fs::path&)>;
[[nodiscard]] const FlushFn& NoFlush();

// Writes bytes to `path` by way of `path`.tmp, flushing before the rename, so that
// an interrupted write leaves the previous contents rather than half of the new
// ones. This is how the journal is written.
[[nodiscard]] bool WriteFileDurably(const fs::path& path,
                                    std::span<const std::uint8_t> bytes,
                                    const FlushFn& flush,
                                    FileStoreError& error);

[[nodiscard]] std::optional<std::vector<std::uint8_t>> ReadFile(const fs::path& path,
                                                                std::uint64_t max_bytes,
                                                                FileStoreError& error);

// Whether this filesystem can tell an executable file from an ordinary one.
//
// Asked of a file this function creates in `scratch_directory` and then removes, not of
// the payload: the question is about the filesystem, and asking the payload would confuse
// "nothing here is executable" with "nothing here can be".
//
// It exists because Windows answers the question wrong rather than not answering it.
// MSVC's std::filesystem models exactly one attribute -- read-only -- and reports
// `perms::all` for every writable file, so every member of a payload packed on Windows
// would be marked executable, including the .pak and .dat files. A flag that is set on
// everything says nothing, and a format whose one flag says nothing on the platform the
// payload is actually built for is worse than a format without it.
//
// A brand-new empty file that already claims to be executable is the tell.
[[nodiscard]] bool ExecuteBitIsMeaningful(const fs::path& scratch_directory);

// Walks `dir` and writes the archive of it to `out`.
//
// The bytes this produces are a function of the tree's contents and nothing else --
// no timestamps, no order the directory happened to be walked in, no compression --
// which is the property ADR 0010 needs, because a client has to reproduce them
// exactly from its own installation years later.
//
// Symbolic links are refused, not followed: a payload that contains one either
// escapes the installation directory or depends on something outside it, and both
// are worse failures than not shipping.
//
// The executable flag is recorded only where ExecuteBitIsMeaningful says it means
// something, which on Windows is nowhere.
//
// Empty directories are not represented and therefore do not survive a round trip.
// Sonora's payload has none, and an archive format that recorded them would need an
// entry that is not a file, which is a second kind of member for no gain.
[[nodiscard]] std::optional<ArchiveHeader> PackDirectory(const fs::path& dir,
                                                         const fs::path& out,
                                                         FileStoreError& error);

// Unpacks `archive` into `dir`, which must not already exist.
//
// Every member's hash is checked as it is written. The archive's own hash was
// already checked against the signed manifest before this was called, so this is
// the second of two checks over the same bytes -- the first says the archive is the
// one the release published, and this one says the file on the disk is the one the
// archive described.
[[nodiscard]] bool UnpackArchive(const fs::path& archive,
                                 const fs::path& dir,
                                 const FlushFn& flush,
                                 FileStoreError& error);

[[nodiscard]] std::optional<Hash256> HashFile(const fs::path& path, FileStoreError& error);

// Removes a directory and everything in it. Separate from the rest because it is
// the one operation here that cannot be undone.
[[nodiscard]] bool RemoveTree(const fs::path& dir, FileStoreError& error);

// The Runner from driver.h, over real directories.
class FileSystemRunner final : public Runner {
 public:
  FileSystemRunner(Layout layout, FlushFn flush);

  [[nodiscard]] bool WriteJournal(const Journal& journal) override;
  [[nodiscard]] Facts Look() override;
  [[nodiscard]] bool Perform(Step step) override;

  [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }

 private:
  Layout layout_;
  FlushFn flush_;
  std::string last_error_;
};

// Reads the journal, or an idle one if there is no file yet.
//
// A file that exists and does not parse is not an idle journal: the second value
// says which happened, and the caller is expected to stop rather than assume.
struct LoadedJournal {
  Journal journal;
  bool readable = true;
};
[[nodiscard]] LoadedJournal LoadJournal(const Layout& layout);

// The launch flag: written by the application when it has shown its window with the
// UI loaded, read by the updater to decide whether the version that wrote it works.
[[nodiscard]] bool WriteLaunchFlag(const Layout& layout,
                                   const Version& version,
                                   const FlushFn& flush);
[[nodiscard]] std::optional<Version> ReadLaunchFlag(const Layout& layout);

}  // namespace sonora::update
