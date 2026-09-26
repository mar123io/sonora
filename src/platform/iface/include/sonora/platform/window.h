#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <sonora/core/window_placement.h>

namespace sonora::platform {

// Sizes are in device-independent pixels. The platform layer owns the
// conversion to physical pixels, because that is exactly the kind of detail
// that differs per OS and must not leak into core or into the UI layer.
struct WindowDesc {
  std::string title = "Sonora";
  int width_dip = 1100;
  int height_dip = 720;
  int min_width_dip = 640;
  int min_height_dip = 480;

  // Where to open, in physical pixels, already decided by
  // core::ResolvePlacement. Empty means "wherever this platform would put a new
  // window", which is what the first run gets.
  //
  // Passed at creation rather than applied afterwards: a window that appears at
  // one place and moves to another is a visible flicker, and on Windows it is
  // also two rounds of WM_DPICHANGED if the two places have different scales.
  std::optional<core::Placement> placement;
};

// The window's client area, in physical pixels: what is left after the frame
// and the title bar, and therefore what anything drawing inside has to fill.
struct SizePx {
  int width = 0;
  int height = 0;
};

// A top-level application window.
//
// Deliberately narrow: everything CEF needs in week 2 is a native handle and a
// resize notification, and everything the shell needs is show/close. Resist
// widening this interface — every method added here must be implemented three
// times, and an interface that is cheap to implement is what makes the macOS
// and Linux ports stay compilable.
class Window {
 public:
  virtual ~Window() = default;

  Window(const Window&) = delete;
  Window& operator=(const Window&) = delete;

  virtual void Show() = 0;
  virtual void Close() = 0;

  // Brings the window to the front, restoring it if it was minimised.
  //
  // Separate from Show() because they answer different questions. Show() is
  // "this window exists now", once, at startup, and on Windows it is also where
  // a remembered maximized state is applied. Raise() is "the user asked for
  // this window again" -- a second launch, a link clicked in a browser, the
  // tray menu -- and on Windows those are entirely different calls.
  virtual void Raise() = 0;

  // HWND on Windows, NSWindow* on macOS. The caller is expected to know which
  // platform it is compiled for; this is the one place where that is fine.
  [[nodiscard]] virtual void* native_handle() const noexcept = 0;

  // Physical pixels per device-independent pixel (1.0 at 96 dpi, 1.5 at 144...).
  [[nodiscard]] virtual float scale_factor() const noexcept = 0;

  // The size of the area a hosted view has to fill, right now.
  //
  // Here because the alternative is what week 2 did: compute it from the
  // *requested* size and the scale factor, and hope the window that was
  // actually created agrees. It did agree, every time, until a window could
  // open at a size somebody had chosen last week -- and then the browser view
  // kept the default size inside a window that had a different one. Ask the
  // window; it knows.
  [[nodiscard]] virtual SizePx client_size() const noexcept = 0;

  // Fired when the user asks to close the window. Setting a handler means the
  // window will NOT close by itself: the handler decides.
  virtual void SetOnClose(std::function<void()> handler) = 0;

  // Fired after the window was resized or moved to a display with a different
  // scale factor. CEF's browser view is repositioned from here in week 2.
  virtual void SetOnResize(std::function<void(int width_px, int height_px)> handler) = 0;

  // Where this window is now, in the form that goes into the settings table.
  //
  // The bounds are the *restored* ones even while the window is maximized --
  // otherwise un-maximizing a restored window would give it the size of the
  // screen it was last maximized on, forever. Windows keeps both; so does this.
  [[nodiscard]] virtual core::SavedPlacement SavedPlacement() const = 0;

 protected:
  Window() = default;
};

// Returns nullptr when no window backend is compiled in (Linux today).
[[nodiscard]] std::unique_ptr<Window> CreateAppWindow(const WindowDesc& desc);

}  // namespace sonora::platform
