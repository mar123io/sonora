#include <sonora/platform/app_main.h>

#include <windows.h>

namespace sonora::platform {
namespace {
HINSTANCE g_instance = nullptr;
}  // namespace

void* NativeInstanceHandle() noexcept {
  return g_instance != nullptr ? g_instance : ::GetModuleHandleW(nullptr);
}

}  // namespace sonora::platform

// Both entry points are defined; the linker keeps the one matching the
// subsystem (console in Debug, windows in Release -- see src/shell/CMakeLists).
int main() {
  return sonora::platform::AppMain();
}

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
  sonora::platform::g_instance = instance;
  return sonora::platform::AppMain();
}
