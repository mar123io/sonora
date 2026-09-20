#include <sonora/platform/paths.h>

#include <windows.h>

#include <string>
#include <vector>

namespace sonora::platform {

std::filesystem::path ExecutablePath() {
  // The path can exceed MAX_PATH on a long-path-aware system, so grow until it
  // fits rather than assuming a buffer size.
  std::vector<wchar_t> buffer(MAX_PATH);
  for (;;) {
    const DWORD written =
        ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (written == 0) {
      return {};
    }
    if (written < buffer.size()) {
      return std::filesystem::path(std::wstring(buffer.data(), written));
    }
    buffer.resize(buffer.size() * 2);
  }
}

std::filesystem::path UserDataDirectory(std::string_view app_name) {
  std::vector<wchar_t> buffer(MAX_PATH);
  DWORD written = ::GetEnvironmentVariableW(L"LOCALAPPDATA", buffer.data(),
                                            static_cast<DWORD>(buffer.size()));
  if (written >= buffer.size()) {
    buffer.resize(written + 1);
    written = ::GetEnvironmentVariableW(L"LOCALAPPDATA", buffer.data(),
                                        static_cast<DWORD>(buffer.size()));
  }
  if (written == 0) {
    return ExecutablePath().parent_path() / std::filesystem::path(std::string(app_name));
  }
  return std::filesystem::path(std::wstring(buffer.data(), written)) /
         std::filesystem::path(std::string(app_name));
}

}  // namespace sonora::platform
