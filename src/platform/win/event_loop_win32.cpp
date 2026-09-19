#include <sonora/platform/event_loop.h>

#include <windows.h>

namespace sonora::platform {

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
