#pragma once

namespace sonora::platform {

// Positions a native child window (an HWND on Windows, an NSView on macOS)
// inside its parent, in physical pixels.
//
// CEF hands back a native handle for the browser view and expects the host to
// keep it sized. Doing that here rather than in the CEF layer is what keeps
// SetWindowPos and friends out of src/shell/ -- see ADR 0002.
void SetChildWindowBounds(void* native_child, int x, int y, int width, int height);

}  // namespace sonora::platform
