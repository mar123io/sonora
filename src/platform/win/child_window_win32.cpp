#include <sonora/platform/child_window.h>

#include <windows.h>

namespace sonora::platform {

void SetChildWindowBounds(void* native_child, int x, int y, int width, int height) {
  auto hwnd = static_cast<HWND>(native_child);
  if (hwnd == nullptr || !::IsWindow(hwnd)) {
    return;
  }
  ::SetWindowPos(hwnd, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}

}  // namespace sonora::platform
