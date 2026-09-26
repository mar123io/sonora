#include <sonora/platform/window.h>

// clang-format off
#include <windows.h>
#include <dwmapi.h>
#include <objbase.h>
// shlobj.h rather than shobjidl.h: SHGetPropertyStoreForWindow is documented in
// shobjidl_core.h, and shlobj.h is the umbrella that pulls in the desktop shell
// family with the partition macros already set the way a desktop application
// needs them. Included directly, the narrower header left the function
// undeclared.
#include <shlobj.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shlwapi.h>
// clang-format on

#include <sonora/platform/event_loop.h>

#include <cstdio>
#include <string>
#include <utility>

namespace sonora::platform {
namespace {

constexpr wchar_t kWindowClassName[] = L"SonoraMainWindow";
constexpr UINT kDefaultDpi = 96;

// DWMWA_USE_IMMERSIVE_DARK_MODE. Declared by name only since Windows 10 20H1
// SDKs; using the literal keeps this compiling against older SDKs too.
constexpr DWORD kDwmUseImmersiveDarkMode = 20;

// Who this program is, as far as the Windows shell is concerned.
//
// It is a made-up string and it only has to be stable and unique: the shell
// uses it to group taskbar buttons, to find a jump list (week 9), and to look
// up what to call the application. The form is the documented one,
// CompanyName.ProductName.
// Unused today, on purpose: see ConfigureShellIdentity. Week 9's jump list and
// week 10's installer both need this exact string, and writing it down once,
// here, is what stops the two from disagreeing later.
[[maybe_unused]] constexpr wchar_t kAppUserModelId[] = L"MarioLizzio.Sonora";

// The string resource in sonora.rc that holds the application's name, and the
// icon resource beside it. Referenced indirectly, by id, because that is what
// the shell asks for -- and because a name in a resource is a name that can be
// translated, which a string compiled into this file is not.
constexpr int kAppNameStringId = 101;
constexpr int kAppIconId = 1;

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

[[nodiscard]] std::wstring ExecutablePathW() {
  std::wstring path(MAX_PATH, L'\0');
  for (;;) {
    const DWORD written =
        ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (written == 0) {
      return {};
    }
    if (written < path.size()) {
      path.resize(written);
      return path;
    }
    path.resize(path.size() * 2);
  }
}

// SHGetPropertyStoreForWindow, resolved from shell32.dll at runtime.
//
// Not a preference. The function is exported by shell32 and documented in
// shobjidl_core.h, but its declaration sits behind the SDK's API-partition
// guards, and in this toolchain's configuration they do not open for it: two
// different umbrella headers left it undeclared while IPropertyStore and every
// PKEY_AppUserModel_* beside it resolved fine. Rather than fight the guards with
// macros -- which would mean deciding, per SDK version, which ones to force --
// this asks the DLL directly for the one symbol that is missing.
//
// The cost is one GetProcAddress per process. The benefit is that it cannot be
// broken again by an SDK update, a toolset change, or the next header that ends
// up included above this one.
using SHGetPropertyStoreForWindowFn = HRESULT(WINAPI*)(HWND, REFIID, void**);

[[nodiscard]] SHGetPropertyStoreForWindowFn ResolvePropertyStoreForWindow() {
  static const SHGetPropertyStoreForWindowFn function = [] {
    // Already loaded: this process links shell32 for CommandLineToArgvW, and no
    // GUI process runs without it. LoadLibrary is the belt to that braces.
    HMODULE shell32 = ::GetModuleHandleW(L"shell32.dll");
    if (shell32 == nullptr) {
      shell32 = ::LoadLibraryW(L"shell32.dll");
    }
    if (shell32 == nullptr) {
      return static_cast<SHGetPropertyStoreForWindowFn>(nullptr);
    }
    return reinterpret_cast<SHGetPropertyStoreForWindowFn>(
        reinterpret_cast<void*>(::GetProcAddress(shell32, "SHGetPropertyStoreForWindow")));
  }();
  return function;
}

void SetStringProperty(IPropertyStore* store,
                       const PROPERTYKEY& key,
                       const std::wstring& value) {
  PROPVARIANT variant;
  if (FAILED(::InitPropVariantFromString(value.c_str(), &variant))) {
    return;
  }
  store->SetValue(key, variant);
  ::PropVariantClear(&variant);
}

// Tells the shell what to call this application, and what to draw next to the
// name.
//
// This is what the system's media panel reads, and the reason it spent an
// evening calling Sonora "Unknown app". A version block gives the *file* a
// description, which Explorer shows in a column and the media panel does not
// look at. What the shell actually resolves is the AppUserModelID, and for an
// unpackaged application with no shortcut in the Start Menu there is nothing to
// resolve it against -- so the documented answer is to put the answer on the
// window itself, in its property store, as relaunch properties.
//
// An installer that creates a shortcut carrying the same AppUserModelID would
// make these redundant (week 10). Until there is one, this is not a workaround:
// it is the path Microsoft documents for exactly this case.
void ConfigureShellIdentity(HWND hwnd) {
  // Said out loud, every run, in one line. Everything this function does is
  // invisible by nature -- properties written into a store nobody reads back --
  // and the symptom of any of it failing is the same four words in the media
  // panel, "Unknown app", which say nothing about which step gave up. An
  // afternoon went into telling those cases apart by guessing; this is the
  // cheaper way.
  const auto report = [](const char* what, HRESULT result) {
    if (SUCCEEDED(result)) {
      std::printf("shell identity: %s\n", what);
    } else {
      std::printf("shell identity: %s failed (0x%08lx)\n", what,
                  static_cast<unsigned long>(result));
    }
    std::fflush(stdout);
  };

  // Note what is NOT called here: SetCurrentProcessExplicitAppUserModelID.
  //
  // An explicit AppUserModelID is a promise that something registers it, and
  // nothing does yet. Windows resolves an id to a name and an icon by finding a
  // shortcut that carries it; with an id nobody has registered, the lookup fails
  // and the shell falls back to nothing -- which is worse than the id it would
  // have derived on its own from the executable's path, because *that* one a
  // plain Start Menu shortcut can satisfy.
  //
  // So the explicit id comes back in week 10, in the installer, together with
  // the shortcut that carries it. The id itself is already chosen and written
  // above, because week 9's jump list needs the same one.

  const std::wstring executable = ExecutablePathW();
  if (executable.empty()) {
    report("no executable path", E_FAIL);
    return;
  }

  const SHGetPropertyStoreForWindowFn property_store_for_window =
      ResolvePropertyStoreForWindow();
  if (property_store_for_window == nullptr) {
    report("SHGetPropertyStoreForWindow not exported by shell32", E_NOTIMPL);
    return;
  }

  IPropertyStore* store = nullptr;
  const HRESULT opened = property_store_for_window(hwnd, IID_PPV_ARGS(&store));
  if (FAILED(opened) || store == nullptr) {
    report("window property store", opened);
    return;
  }

  // Nor a window-level id, for the same reason as the process-level one above:
  // an id the shell cannot resolve is worse than the one it derives from the
  // executable's own path, which a plain shortcut can satisfy.
  SetStringProperty(store, PKEY_AppUserModel_RelaunchCommand, L"\"" + executable + L"\"");
  SetStringProperty(store, PKEY_AppUserModel_RelaunchIconResource,
                    executable + L",-" + std::to_wstring(kAppIconId));

  // The display name is an indirect string: @<file>,-<id>, resolved through the
  // resource table. Checked before it is set, and replaced with the plain name
  // if the resource cannot be loaded -- a name that fails to resolve leaves the
  // panel saying "Unknown app", which is the bug this function exists to fix,
  // and a silent fallback is better than a silent recurrence.
  const std::wstring indirect = L"@" + executable + L",-" + std::to_wstring(kAppNameStringId);
  wchar_t resolved[256] = {};
  const HRESULT loaded =
      ::SHLoadIndirectString(indirect.c_str(), resolved, ARRAYSIZE(resolved), nullptr);
  SetStringProperty(store, PKEY_AppUserModel_RelaunchDisplayNameResource,
                    SUCCEEDED(loaded) ? indirect : std::wstring(L"Sonora"));

  const HRESULT committed = store->Commit();
  store->Release();

  std::printf("shell identity: store=0x%08lx name=%ls commit=0x%08lx\n",
              static_cast<unsigned long>(opened),
              SUCCEEDED(loaded) ? resolved : L"(resource did not load, using a literal)",
              static_cast<unsigned long>(committed));
  std::fflush(stdout);
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
    ConfigureShellIdentity(hwnd_);
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

    // Resource id 1 is the application icon, by convention in this project and
    // by convention in Windows: the first icon in an executable is the one
    // Explorer, the taskbar and the system's media panel use. Loading it here is
    // what puts it on the title bar and in Alt-Tab as well; without it the
    // window gets the generic placeholder even though the file has an icon.
    //
    // LoadImageW rather than LoadIconW so that each of the two sizes comes from
    // the drawing made for it, instead of the large one squashed.
    const HINSTANCE instance = wc.hInstance;
    wc.hIcon = static_cast<HICON>(::LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                               ::GetSystemMetrics(SM_CXICON),
                                               ::GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    wc.hIconSm = static_cast<HICON>(
        ::LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, ::GetSystemMetrics(SM_CXSMICON),
                     ::GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
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
