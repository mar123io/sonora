#pragma once

#include <vector>

#include <sonora/core/window_placement.h>

namespace sonora::platform {

// Every display attached right now, in the virtual desktop's coordinates.
//
// Work areas rather than full bounds, because a window restored under the
// taskbar is as lost as one restored off the screen. Empty when the platform
// has no display backend, or during a session switch -- both of which
// core::ResolvePlacement is written to survive.
//
// A snapshot: monitors are plugged in and unplugged, and this answer is only
// true when it was asked for. It is asked for once, just before the window is
// created.
[[nodiscard]] std::vector<core::Display> Displays();

}  // namespace sonora::platform
