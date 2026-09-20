#pragma once

#include <filesystem>
#include <string_view>

namespace sonora::platform {

// Absolute path of the running executable. Everything the application needs to
// find next to itself (the CEF helper, CEF's resource files) hangs off this,
// never off the current working directory, which a shortcut or a file
// association can set to anything.
[[nodiscard]] std::filesystem::path ExecutablePath();

// Per-user writable directory for this application:
//   %LOCALAPPDATA%\<app_name>        on Windows
//   ~/Library/Application Support/<app_name>  on macOS
//   $XDG_DATA_HOME/<app_name>        on Linux
[[nodiscard]] std::filesystem::path UserDataDirectory(std::string_view app_name);

}  // namespace sonora::platform
