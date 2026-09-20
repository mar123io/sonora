#include <sonora/platform/event_loop.h>

#include <windows.h>

#include <utility>

namespace sonora::platform {
namespace {

constexpr UINT kMsgSchedule = WM_APP + 1;
constexpr UINT kMsgDoWork = WM_APP + 2;
constexpr UINT_PTR kWorkTimerId = 1;
constexpr wchar_t kWorkWindowClass[] = L"SonoraWorkWindow";

WorkCallback g_work_callback;
HWND g_work_window = nullptr;

void RunWork() {
  if (g_work_callback) {
    g_work_callback();
  }
}

LRESULT CALLBACK WorkWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case kMsgSchedule: {
      // ScheduleWork may be called from any thread, but SetTimer only works on
      // a window owned by the calling thread. PostMessage is thread safe, so
      // the request hops here first and the timer is armed on the loop thread.
      const auto delay_ms = static_cast<int64_t>(lparam);
      ::KillTimer(hwnd, kWorkTimerId);
      if (delay_ms <= 0) {
        ::PostMessageW(hwnd, kMsgDoWork, 0, 0);
      } else {
        ::SetTimer(hwnd, kWorkTimerId, static_cast<UINT>(delay_ms), nullptr);
      }
      return 0;
    }

    case kMsgDoWork:
      RunWork();
      return 0;

    case WM_TIMER:
      if (wparam == kWorkTimerId) {
        ::KillTimer(hwnd, kWorkTimerId);
        RunWork();
        return 0;
      }
      break;

    default:
      break;
  }
  return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

HWND EnsureWorkWindow() {
  if (g_work_window != nullptr) {
    return g_work_window;
  }
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = &WorkWndProc;
  wc.hInstance = ::GetModuleHandleW(nullptr);
  wc.lpszClassName = kWorkWindowClass;
  ::RegisterClassExW(&wc);

  // HWND_MESSAGE: a message-only window. It never appears on screen, is not
  // enumerated, and costs nothing beyond its message queue slot.
  g_work_window = ::CreateWindowExW(0, kWorkWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                                    nullptr, wc.hInstance, nullptr);
  return g_work_window;
}

}  // namespace

void SetWorkCallback(WorkCallback callback) {
  g_work_callback = std::move(callback);
  EnsureWorkWindow();
}

void ScheduleWork(int64_t delay_ms) {
  // Deliberately does not create the window: this can run on a background
  // thread, and window creation must happen on the loop thread. SetWorkCallback
  // is what creates it, and it runs first.
  if (g_work_window != nullptr) {
    ::PostMessageW(g_work_window, kMsgSchedule, 0, static_cast<LPARAM>(delay_ms));
  }
}

int RunEventLoop() {
  MSG msg{};
  while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
    ::TranslateMessage(&msg);
    ::DispatchMessageW(&msg);
  }
  return static_cast<int>(msg.wParam);
}

void RequestQuit(int exit_code) {
  ::PostQuitMessage(exit_code);
}

}  // namespace sonora::platform
