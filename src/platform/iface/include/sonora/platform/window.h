#pragma once

#include <functional>
#include <memory>
#include <string>

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

  // HWND on Windows, NSWindow* on macOS. The caller is expected to know which
  // platform it is compiled for; this is the one place where that is fine.
  [[nodiscard]] virtual void* native_handle() const noexcept = 0;

  // Physical pixels per device-independent pixel (1.0 at 96 dpi, 1.5 at 144...).
  [[nodiscard]] virtual float scale_factor() const noexcept = 0;

  // Fired when the user asks to close the window. Setting a handler means the
  // window will NOT close by itself: the handler decides.
  virtual void SetOnClose(std::function<void()> handler) = 0;

  // Fired after the window was resized or moved to a display with a different
  // scale factor. CEF's browser view is repositioned from here in week 2.
  virtual void SetOnResize(std::function<void(int width_px, int height_px)> handler) = 0;

 protected:
  Window() = default;
};

// Returns nullptr when no window backend is compiled in (Linux today).
[[nodiscard]] std::unique_ptr<Window> CreateAppWindow(const WindowDesc& desc);

}  // namespace sonora::platform
