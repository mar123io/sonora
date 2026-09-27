#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "sonora/platform/paths.h"
#include "sonora/update/filestore.h"

namespace sonora::platform {

// What the updater needs and std::filesystem does not have.
//
// It is five functions, and that number is the whole of ADR 0002's claim about this
// week: an update is a signed manifest, an archive format, a binary patch, a journal
// and a crash-safe sequence of renames -- and none of those five things is about an
// operating system. What is left over is here.
//
// src/platform/win/update_host_win32.cpp implements them. macOS and Linux get
// update_host_none.cpp, which answers honestly that there is no updater on those
// platforms yet -- the same shape as displays_none.cpp and the rest, and for the same
// reason: a stub that lies is worse than a platform that says no.

// Where this process was loaded from is NOT here: sonora::platform::ExecutablePath()
// has existed in paths.h since week 2, and this header declared a second one returning
// an optional.
//
// Two declarations of one name differing only in their return type is not an overload,
// and what happened next is worth the four lines. No translation unit included both
// headers, so nothing complained; the Itanium ABI does not mangle the return type, so
// the two definitions became one symbol and GNU ld refused the duplicate; MSVC does
// mangle it, so on Windows they were two distinct functions and it linked. The
// installation directory that the updater moves was therefore decided by which member
// of a static archive the linker happened to pull first.
//
// One name, one definition, in paths.h -- included above. It returns an empty path
// rather than an empty optional when it cannot answer.

// Copies the running executable somewhere outside the installation and returns where.
//
// This is what makes the swap possible at all: Windows will let you rename a running
// executable but not delete one, and a rollback has to delete the tree the running
// process was loaded from. So the updater's first act is to stop being part of what it
// is about to move.
[[nodiscard]] std::optional<std::filesystem::path> CopyExecutableToTemporary(
    std::string_view stem);

// Starts a process and returns immediately. Used twice: the application starts the
// out-of-tree updater and exits, and the updater starts the restored application after
// a rollback.
[[nodiscard]] bool SpawnDetached(const std::filesystem::path& executable,
                                 const std::vector<std::string>& arguments);

[[nodiscard]] std::uint32_t CurrentProcessId();

// Waits for a process to go away. True if it is gone, including if it was already gone
// before the call -- which is the common case and not an error: the updater is started
// as the application exits, and the race between the two is expected.
[[nodiscard]] bool WaitForProcess(std::uint32_t process_id, int timeout_ms);

// The flush that makes a closed file's bytes survive a power cut, as
// sonora::update::filestore wants it: a function of a path.
//
// A path rather than a handle because that is the only spelling that can cross the ADR
// 0002 boundary -- FlushFileBuffers wants a HANDLE and fsync wants a descriptor, and
// neither word may appear in src/update/.
[[nodiscard]] sonora::update::FlushFn DurableFlush();

}  // namespace sonora::platform
