#include <sonora/platform/app_main.h>

#include <windows.h>

#include <shellapi.h>

#include <cstddef>
#include <utility>

namespace sonora::platform {
namespace {

// Parsed once, on first use. The Windows command line is a single string and
// splitting it correctly -- quotes, embedded spaces, backslash escapes -- is
// famously easy to get subtly wrong by hand, so CommandLineToArgvW does it.
std::vector<std::string> ParseCommandLine() {
  std::vector<std::string> arguments;

  int count = 0;
  LPWSTR* wide = ::CommandLineToArgvW(::GetCommandLineW(), &count);
  if (wide == nullptr) {
    return arguments;
  }

  // Skips index 0, the executable path.
  for (int i = 1; i < count; ++i) {
    const int bytes =
        ::WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) {
      continue;
    }
    std::string utf8(static_cast<std::size_t>(bytes - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, utf8.data(), bytes, nullptr, nullptr);
    arguments.push_back(std::move(utf8));
  }

  ::LocalFree(wide);
  return arguments;
}

}  // namespace

const std::vector<std::string>& CommandLineArguments() {
  // UTF-8 on the way out, always. A path the ANSI code page cannot represent
  // is not a rare case in a music library, and it is the kind of bug that only
  // ever reproduces on someone else's machine.
  static const std::vector<std::string> arguments = ParseCommandLine();
  return arguments;
}

}  // namespace sonora::platform
