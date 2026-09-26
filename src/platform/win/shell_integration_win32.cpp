#include <sonora/platform/shell_integration.h>

// clang-format off
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <objbase.h>
#include <shlobj.h>
#include <propkey.h>
#include <propvarutil.h>
// clang-format on

#include "app_identity.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <utility>
#include <vector>

namespace sonora::platform {
namespace {

constexpr wchar_t kTrayClassName[] = L"SonoraTrayWindow";
constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr UINT_PTR kTrayIconId = 1;
constexpr UINT_PTR kSubclassId = 1;

// Menu and thumbnail-button command ids. They share one space because both
// arrive as WM_COMMAND, from two different windows, and one switch is easier to
// keep right than two that must not collide.
enum CommandId : UINT {
  kCommandShow = 0xA001,
  kCommandPlayPause = 0xA002,
  kCommandNext = 0xA003,
  kCommandPrevious = 0xA004,
  kCommandQuit = 0xA005,
};

constexpr int kAppIconId = 1;

[[nodiscard]] std::wstring Widen(const std::string& utf8) {
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

// ---------------------------------------------------------------------------
// The three glyphs on the taskbar's thumbnail toolbar
// ---------------------------------------------------------------------------
// Drawn here, pixel by pixel, rather than shipped as .ico files, for the same
// reason tools/make_icon.py draws the application icon: a shape in code is a
// diff somebody can review, and these three are rectangles and a triangle.
//
// The pixels are written by hand instead of with GDI because GDI has no opinion
// about the alpha channel -- it writes whatever happens to be in the high byte
// -- and an icon with wrong alpha is a black square on the taskbar's dark
// preview. Premultiplied BGRA, straight into the DIB.

enum class Glyph { kPrevious, kPlay, kPause, kNext };

void Fill(std::uint32_t* pixels,
          int width,
          int x0,
          int y0,
          int x1,
          int y1,
          std::uint32_t colour) {
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      pixels[y * width + x] = colour;
    }
  }
}

// A right-pointing triangle inscribed in the box, or a left-pointing one.
void FillTriangle(std::uint32_t* pixels,
                  int width,
                  int x0,
                  int y0,
                  int x1,
                  int y1,
                  bool pointing_right,
                  std::uint32_t colour) {
  const int height = y1 - y0;
  const int span = x1 - x0;
  for (int y = y0; y < y1; ++y) {
    // How far from the vertical centre, as a fraction: the triangle narrows
    // linearly towards the tip.
    const int from_centre = std::abs((2 * (y - y0) - height + 1));
    const int run = span - (span * from_centre) / (height == 0 ? 1 : height);
    if (run <= 0) {
      continue;
    }
    if (pointing_right) {
      Fill(pixels, width, x0, y, x0 + run, y + 1, colour);
    } else {
      Fill(pixels, width, x1 - run, y, x1, y + 1, colour);
    }
  }
}

[[nodiscard]] HICON MakeGlyphIcon(Glyph glyph, int size) {
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(info.bmiHeader);
  info.bmiHeader.biWidth = size;
  // Negative: a top-down DIB, so row 0 is the top one and the arithmetic below
  // reads the way it looks.
  info.bmiHeader.biHeight = -size;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;

  void* bits = nullptr;
  HBITMAP colour_bitmap = ::CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (colour_bitmap == nullptr || bits == nullptr) {
    return nullptr;
  }

  auto* pixels = static_cast<std::uint32_t*>(bits);
  Fill(pixels, size, 0, 0, size, size, 0x00000000);  // transparent

  constexpr std::uint32_t kWhite = 0xFFFFFFFF;  // premultiplied, fully opaque
  const int margin = size / 5;
  const int inner = size - 2 * margin;
  const int bar = (size + 7) / 8;

  switch (glyph) {
    case Glyph::kPlay:
      FillTriangle(pixels, size, margin + bar / 2, margin, size - margin, size - margin, true,
                   kWhite);
      break;
    case Glyph::kPause:
      Fill(pixels, size, margin, margin, margin + bar, size - margin, kWhite);
      Fill(pixels, size, size - margin - bar, margin, size - margin, size - margin, kWhite);
      break;
    case Glyph::kNext:
      FillTriangle(pixels, size, margin, margin, margin + inner - bar, size - margin, true,
                   kWhite);
      Fill(pixels, size, size - margin - bar, margin, size - margin, size - margin, kWhite);
      break;
    case Glyph::kPrevious:
      FillTriangle(pixels, size, margin + bar, margin, size - margin, size - margin, false,
                   kWhite);
      Fill(pixels, size, margin, margin, margin + bar, size - margin, kWhite);
      break;
  }

  // The mask is ignored for a 32-bit colour bitmap with an alpha channel, but
  // CreateIconIndirect still requires one to exist.
  HBITMAP mask = ::CreateBitmap(size, size, 1, 1, nullptr);

  ICONINFO icon{};
  icon.fIcon = TRUE;
  icon.hbmColor = colour_bitmap;
  icon.hbmMask = mask;
  HICON handle = ::CreateIconIndirect(&icon);

  ::DeleteObject(colour_bitmap);
  ::DeleteObject(mask);
  return handle;
}

// ---------------------------------------------------------------------------

class Win32ShellIntegration final : public ShellIntegration {
 public:
  Win32ShellIntegration() {
    // Already done by the window, which is created first; this is the call that
    // makes the dependency explicit rather than lucky. See app_identity.h --
    // the jump list below is stored under this id, so if it were ever wrong
    // here the list would be registered somewhere nobody looks.
    std::printf("shell: app id %ls (0x%08lx)\n", kAppUserModelId,
                static_cast<unsigned long>(SetProcessAppUserModelId()));
  }

  ~Win32ShellIntegration() override { Stop(); }

  bool Start(void* native_window, ShellCommandFn on_command) override {
    app_window_ = static_cast<HWND>(native_window);
    on_command_ = std::move(on_command);
    if (app_window_ == nullptr) {
      return false;
    }

    // Apartment threaded, on the thread that owns the window, because that is
    // what ITaskbarList3 and the jump-list interfaces require. A caller that
    // already initialised COM differently gets RPC_E_CHANGED_MODE, which is not
    // an error here: it means somebody else set the rules and this code works
    // under them.
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // Never uninitialised. The thread is the UI thread and it lives as long as
    // the process; balancing the call would only be correct if nothing else on
    // it held a COM pointer, and CEF does.
    if (FAILED(com) && com != RPC_E_CHANGED_MODE) {
      std::printf("shell: COM unavailable (0x%08lx), no tray and no jump list\n",
                  static_cast<unsigned long>(com));
      return false;
    }

    // Sent to every top-level window when explorer.exe creates the taskbar
    // button -- and again if explorer restarts, which is the case that makes
    // this a registered message rather than a one-off call after Show().
    taskbar_created_message_ = ::RegisterWindowMessageW(L"TaskbarButtonCreated");

    ::SetWindowSubclass(app_window_, &Win32ShellIntegration::Subclass, kSubclassId,
                        reinterpret_cast<DWORD_PTR>(this));
    subclassed_ = true;

    CreateTrayWindow();
    AddTrayIcon();
    return true;
  }

  void Stop() override {
    if (tray_added_) {
      NOTIFYICONDATAW data{};
      data.cbSize = sizeof(data);
      data.hWnd = tray_window_;
      data.uID = static_cast<UINT>(kTrayIconId);
      ::Shell_NotifyIconW(NIM_DELETE, &data);
      tray_added_ = false;
    }
    if (tray_window_ != nullptr) {
      ::SetWindowLongPtrW(tray_window_, GWLP_USERDATA, 0);
      ::DestroyWindow(tray_window_);
      tray_window_ = nullptr;
    }
    if (subclassed_ && app_window_ != nullptr) {
      ::RemoveWindowSubclass(app_window_, &Win32ShellIntegration::Subclass, kSubclassId);
      subclassed_ = false;
    }
    for (HICON& icon : thumb_icons_) {
      if (icon != nullptr) {
        ::DestroyIcon(icon);
        icon = nullptr;
      }
    }
    if (tray_icon_ != nullptr) {
      ::DestroyIcon(tray_icon_);
      tray_icon_ = nullptr;
    }
    on_command_ = nullptr;
    app_window_ = nullptr;
  }

  void SetState(const ShellState& state) override {
    const bool was_playing = state_.playing;
    state_ = state;

    if (tray_added_) {
      NOTIFYICONDATAW data{};
      data.cbSize = sizeof(data);
      data.hWnd = tray_window_;
      data.uID = static_cast<UINT>(kTrayIconId);
      data.uFlags = NIF_TIP | NIF_SHOWTIP;
      const std::wstring tip = Widen(state.tooltip.empty() ? "Sonora" : state.tooltip);
      // The field is 128 wide characters including the terminator, and a long
      // album title reaches that. Truncating here beats whatever wcscpy would
      // have done.
      const size_t room = (sizeof(data.szTip) / sizeof(wchar_t)) - 1;
      const size_t count = tip.size() < room ? tip.size() : room;
      std::copy_n(tip.begin(), count, data.szTip);
      data.szTip[count] = L'\0';
      ::Shell_NotifyIconW(NIM_MODIFY, &data);
    }

    if (thumb_buttons_added_ && was_playing != state.playing) {
      UpdateThumbButtons();
    }
  }

  void SetJumpList(const std::vector<JumpListEntry>& entries) override {
    if (app_window_ == nullptr) {
      return;
    }

    ICustomDestinationList* list = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_DestinationList, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&list)))) {
      return;
    }

    UINT slots = 0;
    IObjectArray* removed = nullptr;
    // BeginList hands back what the user has removed from the list by hand.
    // Re-adding one of those makes CommitList fail outright, which is how an
    // application that ignores this ends up with no jump list at all rather
    // than one item fewer. Nothing here re-adds: the list is rebuilt from a
    // store, and an entry the user threw away simply reappears next time it is
    // played. Honest, and documented rather than silently wrong.
    if (FAILED(list->BeginList(&slots, IID_PPV_ARGS(&removed)))) {
      list->Release();
      return;
    }
    if (removed != nullptr) {
      removed->Release();
    }

    IObjectCollection* collection = nullptr;
    if (SUCCEEDED(::CoCreateInstance(CLSID_EnumerableObjectCollection, nullptr,
                                     CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&collection)))) {
      const std::wstring executable = ExecutablePathW();
      UINT added = 0;
      for (const JumpListEntry& entry : entries) {
        if (added >= slots) {
          break;  // the shell decides how many fit; asking for more fails the commit
        }
        IShellLinkW* link = MakeLink(executable, entry);
        if (link == nullptr) {
          continue;
        }
        if (SUCCEEDED(collection->AddObject(link))) {
          ++added;
        }
        link->Release();
      }

      IObjectArray* array = nullptr;
      if (added > 0 && SUCCEEDED(collection->QueryInterface(IID_PPV_ARGS(&array)))) {
        // A category of our own, rather than AddUserTasks: these are documents
        // the user opened, not commands the application offers, and the two
        // have different places in the menu.
        if (FAILED(list->AppendCategory(L"Recently played", array))) {
          // Falls back rather than giving up: AppendCategory refuses when the
          // application is not registered to handle what its items launch,
          // and a list in the wrong section beats no list.
          static_cast<void>(list->AddUserTasks(array));
        }
        array->Release();
      }
      collection->Release();
    }

    const HRESULT committed = list->CommitList();
    if (FAILED(committed)) {
      std::printf("shell: jump list not committed (0x%08lx)\n",
                  static_cast<unsigned long>(committed));
    }
    list->Release();
  }

  [[nodiscard]] std::string description() const override {
    return "Win32 tray, taskbar thumbnail buttons and jump list";
  }

 private:
  [[nodiscard]] static IShellLinkW* MakeLink(const std::wstring& executable,
                                             const JumpListEntry& entry) {
    IShellLinkW* link = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&link)))) {
      return nullptr;
    }

    link->SetPath(executable.c_str());
    link->SetArguments(Widen(entry.arguments).c_str());
    link->SetDescription(Widen(entry.description).c_str());
    link->SetIconLocation(executable.c_str(), 0);

    // The visible text of a jump-list item is PKEY_Title on its property
    // store, not the link's description -- the description becomes the
    // tooltip. An item without a title shows up blank.
    IPropertyStore* properties = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&properties)))) {
      PROPVARIANT title;
      if (SUCCEEDED(::InitPropVariantFromString(Widen(entry.title).c_str(), &title))) {
        static_cast<void>(properties->SetValue(PKEY_Title, title));
        static_cast<void>(properties->Commit());
        ::PropVariantClear(&title);
      }
      properties->Release();
    }
    return link;
  }

  void CreateTrayWindow() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &Win32ShellIntegration::TrayWndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = kTrayClassName;
    ::RegisterClassExW(&wc);

    // Message-only: the tray icon needs a window to send its clicks to, not a
    // window anybody sees.
    tray_window_ = ::CreateWindowExW(0, kTrayClassName, L"Sonora", 0, 0, 0, 0, 0, HWND_MESSAGE,
                                     nullptr, wc.hInstance, this);
  }

  void AddTrayIcon() {
    if (tray_window_ == nullptr) {
      return;
    }
    tray_icon_ = static_cast<HICON>(::LoadImageW(
        ::GetModuleHandleW(nullptr), MAKEINTRESOURCEW(kAppIconId), IMAGE_ICON,
        ::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = tray_window_;
    data.uID = static_cast<UINT>(kTrayIconId);
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = kTrayCallbackMessage;
    data.hIcon = tray_icon_;
    ::wcscpy_s(data.szTip, L"Sonora");
    tray_added_ = ::Shell_NotifyIconW(NIM_ADD, &data) != FALSE;

    if (tray_added_) {
      data.uVersion = NOTIFYICON_VERSION_4;
      ::Shell_NotifyIconW(NIM_SETVERSION, &data);
    }
  }

  void ShowTrayMenu() {
    HMENU menu = ::CreatePopupMenu();
    if (menu == nullptr) {
      return;
    }
    ::AppendMenuW(menu, MF_STRING, kCommandShow, L"Show Sonora");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kCommandPlayPause, state_.playing ? L"Pause" : L"Play");
    ::AppendMenuW(menu, MF_STRING, kCommandPrevious, L"Previous");
    ::AppendMenuW(menu, MF_STRING, kCommandNext, L"Next");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kCommandQuit, L"Quit");

    POINT cursor{};
    ::GetCursorPos(&cursor);

    // The two lines that everybody leaves out and then wonders why the menu
    // will not go away when clicked elsewhere: the menu belongs to the
    // foreground window, and the null message after it is what lets it close.
    ::SetForegroundWindow(tray_window_);
    const int chosen = ::TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD, cursor.x,
                                        cursor.y, 0, tray_window_, nullptr);
    ::PostMessageW(tray_window_, WM_NULL, 0, 0);
    ::DestroyMenu(menu);

    if (chosen != 0) {
      Dispatch(static_cast<UINT>(chosen));
    }
  }

  void Dispatch(UINT command) {
    if (!on_command_) {
      return;
    }
    switch (command) {
      case kCommandShow:
        on_command_(ShellCommand::kShowWindow);
        break;
      case kCommandPlayPause:
        on_command_(ShellCommand::kTogglePlayPause);
        break;
      case kCommandNext:
        on_command_(ShellCommand::kNext);
        break;
      case kCommandPrevious:
        on_command_(ShellCommand::kPrevious);
        break;
      case kCommandQuit:
        on_command_(ShellCommand::kQuit);
        break;
      default:
        break;
    }
  }

  void AddThumbButtons() {
    ITaskbarList3* taskbar = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&taskbar)))) {
      return;
    }
    if (FAILED(taskbar->HrInit())) {
      taskbar->Release();
      return;
    }

    const int size = ::GetSystemMetrics(SM_CXSMICON);
    thumb_icons_[0] = MakeGlyphIcon(Glyph::kPrevious, size);
    thumb_icons_[1] = MakeGlyphIcon(state_.playing ? Glyph::kPause : Glyph::kPlay, size);
    thumb_icons_[2] = MakeGlyphIcon(Glyph::kNext, size);

    THUMBBUTTON buttons[3]{};
    FillButton(buttons[0], kCommandPrevious, thumb_icons_[0], L"Previous");
    FillButton(buttons[1], kCommandPlayPause, thumb_icons_[1],
               state_.playing ? L"Pause" : L"Play");
    FillButton(buttons[2], kCommandNext, thumb_icons_[2], L"Next");

    // ThumbBarAddButtons may be called exactly once per window, ever. Every
    // later change goes through ThumbBarUpdateButtons, which is why the
    // play/pause icon is swapped rather than the button being replaced.
    thumb_buttons_added_ = SUCCEEDED(taskbar->ThumbBarAddButtons(app_window_, 3, buttons));
    taskbar->Release();
  }

  void UpdateThumbButtons() {
    ITaskbarList3* taskbar = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&taskbar)))) {
      return;
    }
    if (FAILED(taskbar->HrInit())) {
      taskbar->Release();
      return;
    }

    HICON replacement = MakeGlyphIcon(state_.playing ? Glyph::kPause : Glyph::kPlay,
                                      ::GetSystemMetrics(SM_CXSMICON));
    if (replacement != nullptr) {
      if (thumb_icons_[1] != nullptr) {
        ::DestroyIcon(thumb_icons_[1]);
      }
      thumb_icons_[1] = replacement;
    }

    THUMBBUTTON buttons[3]{};
    FillButton(buttons[0], kCommandPrevious, thumb_icons_[0], L"Previous");
    FillButton(buttons[1], kCommandPlayPause, thumb_icons_[1],
               state_.playing ? L"Pause" : L"Play");
    FillButton(buttons[2], kCommandNext, thumb_icons_[2], L"Next");
    static_cast<void>(taskbar->ThumbBarUpdateButtons(app_window_, 3, buttons));
    taskbar->Release();
  }

  static void FillButton(THUMBBUTTON& button, UINT id, HICON icon, const wchar_t* tip) {
    button.dwMask = static_cast<THUMBBUTTONMASK>(THB_ICON | THB_TOOLTIP | THB_FLAGS);
    button.iId = id;
    button.hIcon = icon;
    button.dwFlags = THBF_ENABLED;
    ::wcscpy_s(button.szTip, tip);
  }

  static LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_NCCREATE) {
      auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
      ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }

    auto* self =
        reinterpret_cast<Win32ShellIntegration*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self != nullptr && msg == kTrayCallbackMessage) {
      // With NOTIFYICON_VERSION_4 the event is in the low word of lParam and
      // the cursor position is in wParam, which is the opposite of what every
      // older example does.
      switch (LOWORD(lparam)) {
        case WM_LBUTTONUP:
          self->Dispatch(kCommandShow);
          return 0;
        case WM_CONTEXTMENU:
        case WM_RBUTTONUP:
          self->ShowTrayMenu();
          return 0;
        default:
          break;
      }
    }
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }

  // The application window's messages, seen without window_win32.cpp having to
  // know this file exists. A subclass rather than a hook the window offers: the
  // two things needed from it -- the taskbar button appearing, and WM_COMMAND
  // from a thumbnail button -- are both shell concerns, and putting them in the
  // window would mean every platform's window grew a shell-shaped hole.
  static LRESULT CALLBACK
  Subclass(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR reference) {
    auto* self = reinterpret_cast<Win32ShellIntegration*>(reference);
    if (self != nullptr) {
      if (msg == self->taskbar_created_message_ && self->taskbar_created_message_ != 0) {
        self->AddThumbButtons();
      } else if (msg == WM_COMMAND && HIWORD(wparam) == THBN_CLICKED) {
        self->Dispatch(LOWORD(wparam));
        return 0;
      }
    }
    return ::DefSubclassProc(hwnd, msg, wparam, lparam);
  }

  HWND app_window_ = nullptr;
  HWND tray_window_ = nullptr;
  HICON tray_icon_ = nullptr;
  HICON thumb_icons_[3]{};
  UINT taskbar_created_message_ = 0;
  bool tray_added_ = false;
  bool thumb_buttons_added_ = false;
  bool subclassed_ = false;
  ShellState state_;
  ShellCommandFn on_command_;
};

}  // namespace

std::unique_ptr<ShellIntegration> CreateShellIntegration() {
  return std::make_unique<Win32ShellIntegration>();
}

}  // namespace sonora::platform
