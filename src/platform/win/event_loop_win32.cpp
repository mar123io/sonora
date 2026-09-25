#include <sonora/platform/event_loop.h>

#include <windows.h>

#include <cstdint>
#include <utility>

namespace sonora::platform {
namespace {

constexpr UINT kMsgSchedule = WM_APP + 1;
constexpr UINT kMsgDoWork = WM_APP + 2;
constexpr UINT_PTR kWorkTimerId = 1;
constexpr wchar_t kWorkWindowClass[] = L"SonoraWorkWindow";

// The safety net, at roughly 30 Hz. See DoWork for why a pump that only runs
// when it is asked to is a pump that can stop for good.
constexpr int kFallbackDelayMs = 32;

WorkCallback g_work_callback;
HWND g_work_window = nullptr;

// Loop-thread state. Plain bools on purpose: every one of them is read and
// written only inside the window procedure, which is one thread by
// construction. Making them atomic would suggest otherwise.
bool g_working = false;    // inside the work callback right now
bool g_reentered = false;  // ...and something asked for more work while we were
bool g_timer_pending = false;

void ArmTimer(HWND hwnd, int delay_ms) {
  ::SetTimer(hwnd, kWorkTimerId, static_cast<UINT>(delay_ms), nullptr);
  g_timer_pending = true;
}

void CancelTimer(HWND hwnd) {
  if (g_timer_pending) {
    ::KillTimer(hwnd, kWorkTimerId);
    g_timer_pending = false;
  }
}

// One turn of CEF's message loop, and the two rules that make it survivable.
//
// **It must never run inside itself.** CefDoMessageLoopWork runs CEF tasks, and
// a CEF task can pump Windows messages of its own -- a menu, a drag, anything
// that opens a nested modal loop. That nested loop dispatches our own timer
// message, which would call this again from inside the call it is already in.
// CEF documents that as not supported. So a re-entrant request is not run: it
// is remembered, and reposted the moment the outer call returns.
//
// **It must not depend on being asked.** This is the bug that cost an evening.
// The pump used to do exactly what CEF's contract says -- CEF calls
// OnScheduleMessagePumpWork, we pump once -- and nothing more. Which works
// until one wake-up goes missing: a timer killed by a schedule that then lost
// its message, a nested loop swallowing a post, anything. After that CEF is
// waiting to be pumped, nothing is going to pump it, and there is no second
// path back: the window keeps its last painted frame, the page keeps running in
// the renderer process and times out its own calls one by one, and the audio
// keeps playing, because none of those live on this thread. An application that
// is alive everywhere except where the clicks arrive.
//
// The fallback timer is what makes that unreachable. When CEF has not asked for
// anything sooner, the pump wakes itself in 32 ms anyway, so the worst a lost
// wake-up can cost is one frame of latency rather than the rest of the session.
// It is what cefclient's own external pump does, and the reason is this one.
void DoWork(HWND hwnd) {
  if (g_working) {
    g_reentered = true;
    return;
  }

  g_working = true;
  g_reentered = false;
  if (g_work_callback) {
    g_work_callback();
  }
  g_working = false;

  if (g_reentered) {
    // Work was discarded while we were inside. Run it as soon as this returns
    // to the loop, rather than waiting for the next timer.
    g_reentered = false;
    ::PostMessageW(hwnd, kMsgDoWork, 0, 0);
    return;
  }

  if (!g_timer_pending) {
    ArmTimer(hwnd, kFallbackDelayMs);
  }
}

LRESULT CALLBACK WorkWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case kMsgSchedule: {
      // ScheduleWork may be called from any thread, but SetTimer only works on
      // a window owned by the calling thread. PostMessage is thread safe, so
      // the request hops here first and the timer is armed on the loop thread.
      const auto delay_ms = static_cast<std::int64_t>(lparam);
      CancelTimer(hwnd);
      if (delay_ms <= 0) {
        DoWork(hwnd);
      } else {
        ArmTimer(hwnd, static_cast<int>(delay_ms));
      }
      return 0;
    }

    case kMsgDoWork:
      CancelTimer(hwnd);
      DoWork(hwnd);
      return 0;

    case WM_TIMER:
      if (wparam == kWorkTimerId) {
        CancelTimer(hwnd);
        DoWork(hwnd);
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
  const bool stopping = callback == nullptr;
  g_work_callback = std::move(callback);
  if (stopping) {
    // Teardown: stop the heartbeat rather than leaving a timer firing into a
    // callback that is no longer there.
    if (g_work_window != nullptr) {
      CancelTimer(g_work_window);
    }
    return;
  }
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
