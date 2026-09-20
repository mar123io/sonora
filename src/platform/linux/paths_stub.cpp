#include <sonora/platform/paths.h>

#include <cstdlib>
#include <string>

namespace sonora::platform {

std::filesystem::path ExecutablePath() {
  std::error_code ec;
  auto path = std::filesystem::read_symlink("/proc/self/exe", ec);
  return ec ? std::filesystem::path{} : path;
}

std::filesystem::path UserDataDirectory(std::string_view app_name) {
  if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg != nullptr && *xdg != '\0') {
    return std::filesystem::path(xdg) / std::string(app_name);
  }
  if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
    return std::filesystem::path(home) / ".local" / "share" / std::string(app_name);
  }
  return std::filesystem::path(".") / std::string(app_name);
}

}  // namespace sonora::platform
