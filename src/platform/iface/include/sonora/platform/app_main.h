#pragma once

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

}  // namespace sonora::platform
