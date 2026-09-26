#pragma once

#include <optional>
#include <string>
#include <vector>

namespace sonora::core {

// Screen geometry in physical pixels, in the virtual desktop's coordinates --
// which on Windows means the primary display starts at (0, 0) and a display to
// its left has negative x. Signed, therefore, and not by accident.
struct Rect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;

  [[nodiscard]] int right() const noexcept { return x + width; }
  [[nodiscard]] int bottom() const noexcept { return y + height; }
  [[nodiscard]] bool empty() const noexcept { return width <= 0 || height <= 0; }
  [[nodiscard]] bool operator==(const Rect& other) const noexcept {
    return x == other.x && y == other.y && width == other.width && height == other.height;
  }
};

[[nodiscard]] int IntersectionArea(const Rect& a, const Rect& b) noexcept;

// One display, as the platform layer reports it.
//
// work_area rather than the full bounds: restoring a window under the taskbar
// is the same bug as restoring it off the screen, only harder to notice.
struct Display {
  Rect work_area;
  bool primary = false;
  // Physical pixels per device-independent pixel. 1.0 at 96 dpi, 1.5 at 144.
  float scale = 1.0F;
};

// What was written down when the window last closed.
struct SavedPlacement {
  Rect bounds;
  bool maximized = false;
  // The scale of the display the window was on when this was saved. Without
  // it, a window saved on a 100% display and restored on a 150% one comes back
  // two thirds of its physical size -- the pixels are the same number, and the
  // pixels are smaller.
  float scale = 1.0F;
};

struct PlacementRequest {
  std::optional<SavedPlacement> saved;
  int default_width = 1100;
  int default_height = 720;
  int min_width = 640;
  int min_height = 480;
};

struct Placement {
  Rect bounds;
  bool maximized = false;
};

// Decides where the window opens.
//
// The guarantee, and the reason this is a function with tests rather than
// twenty lines inside the Win32 window: **the result is always entirely inside
// some display's work area.** A monitor that was unplugged, a laptop undocked,
// a resolution that shrank, a settings file somebody edited by hand -- none of
// them can produce a window the user cannot reach. The price is that a window
// deliberately left hanging over an edge is pulled back in, which is a trade
// made knowingly: the failure it prevents is unrecoverable without editing a
// file, and the one it causes is fixed by dragging.
//
// Displays may be empty (the platform reported nothing, which happens during a
// session switch); the default size is then used as-is.
[[nodiscard]] Placement ResolvePlacement(const PlacementRequest& request,
                                         const std::vector<Display>& displays);

// The stored form, kept deliberately boring: six numbers separated by spaces,
// parsed back leniently.
//
// It lives in a one-line file of its own next to the two databases, and not in
// either of them, for a reason that is ordering rather than principle: the
// window is created before CEF starts, and both stores are opened after. A
// preference that has to be read before anything else exists is a preference
// that should not need a database to be open. Losing it costs one
// default-sized window, so no durability is being given up.
[[nodiscard]] std::string SerializePlacement(const SavedPlacement& placement);
[[nodiscard]] std::optional<SavedPlacement> ParsePlacement(const std::string& text);

}  // namespace sonora::core
