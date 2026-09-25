#pragma once

#include <string>
#include <vector>

namespace sonora::platform {

// The application defines this; the platform layer owns the real entry point
// (main, wWinMain, NSApplicationMain...) and calls it.
//
// This exists so that src/shell/ contains no conditional compilation on the
// operating system, which ADR 0002 forbids. The entry point signature is one of
// the most platform-specific things there is, so it belongs here.
int AppMain();

// HINSTANCE on Windows, nullptr elsewhere. Needed to build CefMainArgs.
[[nodiscard]] void* NativeInstanceHandle() noexcept;

// The process arguments as UTF-8, without the executable name.
//
// AppMain() takes none, because the shape of "the arguments" is itself
// platform-specific: Windows hands the process one string and expects it to be
// split with CommandLineToArgvW, in UTF-16. Converting once, here, keeps that
// out of src/shell/ and means everything above this line sees plain UTF-8.
//
// Parsed on first call and cached; the result never changes during a run.
[[nodiscard]] const std::vector<std::string>& CommandLineArguments();

}  // namespace sonora::platform
