#include <sonora/platform/displays.h>

// No display enumeration on this platform yet.
//
// An empty list is a case core::ResolvePlacement already handles -- it is the
// same answer Windows gives during a session switch -- so the window opens at
// its default size and nothing has to know why.

namespace sonora::platform {

std::vector<core::Display> Displays() {
  return {};
}

}  // namespace sonora::platform
