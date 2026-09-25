#include <sonora/platform/app_main.h>

#include <crt_externs.h>

namespace sonora::platform {

const std::vector<std::string>& CommandLineArguments() {
  // No wWinMain equivalent to capture argv from, and no globals set by a main
  // this layer owns: on macOS the process arguments are readable from the C
  // runtime itself, which is what _NSGetArgv is for.
  static const std::vector<std::string> arguments = [] {
    std::vector<std::string> parsed;
    const int count = *_NSGetArgc();
    char** values = *_NSGetArgv();
    for (int i = 1; i < count; ++i) {  // index 0 is the executable
      parsed.emplace_back(values[i]);
    }
    return parsed;
  }();
  return arguments;
}

}  // namespace sonora::platform
