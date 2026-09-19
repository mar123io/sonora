#include <sonora/platform/window.h>

// clang-format off
#include <windows.h>
#include <dwmapi.h>
// clang-format on

#include <sonora/platform/event_loop.h>

#include <string>
#include <utility>

namespace sonora::platform {
namespace {

constexpr wchar_t kWindowClassName[] = L"SonoraMainWindow";
constexpr UINT kDefaultDpi = 96;

// DWMWA_USE_IMMERSIVE_DARK_MODE. Declared by name only since Windows 10 20H1
// SDKs; using the literal keeps this compiling against older SDKs too.
constexpr DWORD kDwmUseImmersiveDarkMode = 20;

std::wstring Widen(const std::string& utf8) {
  if (utf8.empty()) {
    return {};
  }
  const int needed =
      ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring wide(static_cast<size_t>(needed), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(),
                        needed);
  return wide;
}

int ScaleForDpi(int value_dip, UINT dpi) {
  return ::MulDiv(value_dip, static_cast<int>(dpi), static_cast<int>(kDefaultDpi));
}

class Win32Window final : public Window {
 public:
  explicit Win32Window(const WindowDesc& desc) : desc_(desc) {
    RegisterWindowClass();

    // The window is created at the *system* dpi and then corrected by the
    // WM_DPICHANGED that Windows sends when it lands on its actual monitor.
    // Creating it at 96 dpi and resizing afterwards is the usual source of a
    // visible "jump" on high-dpi displays.
    const UINT dpi = ::GetDpiForSystem();
    RECT rect{0, 0, ScaleForDpi(desc.width_dip, dpi), ScaleForDpi(desc.height_dip, dpi)};
    const DWORD style = WS_OVERLAPPEDWINDOW;
    ::AdjustWindowRectExForDpi(&rect, style, FALSE, 0, dpi);

    hwnd_ =
        ::CreateWindowExW(0, kWindowClassName, Widen(desc.title).c_str(), style, CW_USEDEFAULT,
                          CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
                          nullptr, nullptr, ::GetModuleHandleW(nullptr), this);
    if (hwnd_ == nullptr) {
      return;
    }

    dpi_ = ::GetDpiForWindow(hwnd_);
    ApplyDarkTitleBar();
  }

  ~Win32Window() override {
    if (hwnd_ != nullptr) {
      ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
      ::DestroyWindow(hwnd_);
      hwnd_ = nullptr;
    }
  }

  void Show() override {
    if (hwnd_ != nullptr) {
      ::ShowWindow(hwnd_, SW_SHOW);
      ::UpdateWindow(hwnd_);
    }
  }

  void Close() override {
    if (hwnd_ != nullptr) {
      ::DestroyWindow(hwnd_);
      hwnd_ = nullptr;
    }
  }

  void* native_handle() const noexcept override { return hwnd_; }

  float scale_factor() const noexcept override {
    return static_cast<float>(dpi_) / static_cast<float>(kDefaultDpi);
  }

  void SetOnClose(std::function<void()> handler) override { on_close_ = std::move(handler); }

  void SetOnResize(std::function<void(int, int)> handler) override {
    on_resize_ = std::move(handler);
  }

 private:
  static void RegisterWindowClass() {
    static bool registered = false;
    if (registered) {
      return;
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &Win32Window::WndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    // No background brush: we paint every pixel ourselves, which removes the
    // white flash between window creation and the first frame. CEF will take
    // over the client area in week 2.
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kWindowClassName;
    ::RegisterClassExW(&wc);
    registered = true;
  }

  void ApplyDarkTitleBar() const {
    const BOOL dark = TRUE;
    ::DwmSetWindowAttribute(hwnd_, kDwmUseImmersiveDarkMode, &dark, sizeof(dark));
  }

  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_NCCREATE) {
      auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
      ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }

    auto* self = reinterpret_cast<Win32Window*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self != nullptr) {
      return self->HandleMessage(hwnd, msg, wparam, lparam);
    }
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }

  LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
      case WM_DPICHANGED: {
        dpi_ = HIWORD(wparam);
        // Windows hands us the rect the window should occupy on the new
        // monitor. Using it verbatim is the only way to drag a window between
        // displays of different scale without it drifting.
        const auto* suggested = reinterpret_cast<const RECT*>(lparam);
        ::SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                       suggested->right - suggested->left, suggested->bottom - suggested->top,
                       SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
      }

      case WM_GETMINMAXINFO: {
        RECT rect{0, 0, ScaleForDpi(desc_.min_width_dip, dpi_),
                  ScaleForDpi(desc_.min_height_dip, dpi_)};
        ::AdjustWindowRectExForDpi(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi_);
        auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
        info->ptMinTrackSize.x = rect.right - rect.left;
        info->ptMinTrackSize.y = rect.bottom - rect.top;
        return 0;
      }

      case WM_SIZE: {
        if (on_resize_ && wparam != SIZE_MINIMIZED) {
          on_resize_(LOWORD(lparam), HIWORD(lparam));
        }
        return 0;
      }

      case WM_ERASEBKGND:
        return 1;  // handled; prevents flicker

      case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = ::BeginPaint(hwnd, &ps);
        HBRUSH brush = ::CreateSolidBrush(RGB(18, 18, 18));
        ::FillRect(dc, &ps.rcPaint, brush);
        ::DeleteObject(brush);
        ::EndPaint(hwnd, &ps);
        return 0;
      }

      case WM_CLOSE: {
        // A handler means the application decides when to close (unsaved
        // state, playback still running, an update waiting to be applied).
        if (on_close_) {
          on_close_();
          return 0;
        }
        break;
      }

      case WM_DESTROY: {
        hwnd_ = nullptr;
        RequestQuit(0);
        return 0;
      }

      default:
        break;
    }
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }

  WindowDesc desc_;
  HWND hwnd_ = nullptr;
  UINT dpi_ = kDefaultDpi;
  std::function<void()> on_close_;
  std::function<void(int, int)> on_resize_;
};

}  // namespace

std::unique_ptr<Window> CreateAppWindow(const WindowDesc& desc) {
  auto window = std::make_unique<Win32Window>(desc);
  if (window->native_handle() == nullptr) {
    return nullptr;
  }
  return window;
}

}  // namespace sonora::platform
