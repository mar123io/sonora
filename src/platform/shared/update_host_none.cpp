#include "sonora/platform/update_host.h"

namespace sonora::platform {

// macOS and Linux do not ship an updater yet, and this file says so rather than
// pretending.
//
// It would be easy to write: /proc/self/exe or _NSGetExecutablePath, a copy, a fork, a
// waitpid, an fsync. Four of the five are ten lines. The reason none of them is here is
// that an updater is not those five functions -- it is a signed release channel, a
// packaging pipeline, an installer to replace and a swap whose failure modes somebody
// has tested on the platform in question. Sonora has all of that for Windows and none
// of it for the other two, and a platform layer that answered these calls would make an
// application that looks like it can update itself and cannot.
//
// What is real on macOS and Linux is everything in src/update/: the manifest, the
// signature, the archive, the patch and the journal are built and tested by the CI jobs
// on both, including the end-to-end tests, which is why the day this file becomes five
// real functions is a short day.

std::optional<std::filesystem::path> CopyExecutableToTemporary(std::string_view) {
  return std::nullopt;
}

bool SpawnDetached(const std::filesystem::path&, const std::vector<std::string>&) {
  return false;
}

std::uint32_t CurrentProcessId() {
  return 0;
}

bool WaitForProcess(std::uint32_t, int) {
  return true;
}

sonora::update::FlushFn DurableFlush() {
  // Not durable, and honest about it: sonora::update::NoFlush() is documented as
  // correct in every respect except the one only a power cut can tell.
  return sonora::update::NoFlush();
}

}  // namespace sonora::platform
