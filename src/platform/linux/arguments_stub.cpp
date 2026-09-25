#include <sonora/platform/app_main.h>

#include <fstream>

namespace sonora::platform {

const std::vector<std::string>& CommandLineArguments() {
  // /proc/self/cmdline rather than a captured argv, for the same reason as the
  // macOS file: this layer owns no entry point on Linux yet. The arguments are
  // NUL-separated there, which is the one thing worth knowing about it.
  static const std::vector<std::string> arguments = [] {
    std::vector<std::string> parsed;
    std::ifstream cmdline("/proc/self/cmdline", std::ios::binary);
    std::string value;
    bool first = true;
    while (std::getline(cmdline, value, '\0')) {
      if (first) {
        first = false;  // the executable
        continue;
      }
      if (!value.empty()) {
        parsed.push_back(value);
      }
    }
    return parsed;
  }();
  return arguments;
}

}  // namespace sonora::platform
