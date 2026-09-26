#include <sonora/platform/displays.h>

// clang-format off
#include <windows.h>
#include <shellscalingapi.h>
// clang-format on

namespace sonora::platform {
namespace {

constexpr UINT kDefaultDpi = 96;

// GetDpiForMonitor lives in shcore.dll, which this project does not link: the
// application's per-monitor v2 awareness comes from the manifest, and linking a
// DLL for one function that has a documented fallback is a dependency bought
// cheaply and paid for at every start. Resolved once, and if it is missing --
// which it is not on any supported Windows, but a resolved symbol that is
// checked is worth more than an assumption that is not -- every display is
// reported at the system scale, which is exactly what a per-monitor-unaware
// program would see anyway.
using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, MONITOR_DPI_TYPE, UINT*, UINT*);

[[nodiscard]] GetDpiForMonitorFn ResolveGetDpiForMonitor() {
  static const GetDpiForMonitorFn function = [] {
    HMODULE shcore = ::LoadLibraryW(L"shcore.dll");
    if (shcore == nullptr) {
      return static_cast<GetDpiForMonitorFn>(nullptr);
    }
    return reinterpret_cast<GetDpiForMonitorFn>(
        reinterpret_cast<void*>(::GetProcAddress(shcore, "GetDpiForMonitor")));
  }();
  return function;
}

[[nodiscard]] float ScaleOf(HMONITOR monitor) {
  const GetDpiForMonitorFn get_dpi = ResolveGetDpiForMonitor();
  if (get_dpi != nullptr) {
    UINT dpi_x = 0;
    UINT dpi_y = 0;
    if (SUCCEEDED(get_dpi(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y)) && dpi_x != 0) {
      return static_cast<float>(dpi_x) / static_cast<float>(kDefaultDpi);
    }
  }
  return static_cast<float>(::GetDpiForSystem()) / static_cast<float>(kDefaultDpi);
}

BOOL CALLBACK OnMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  if (::GetMonitorInfoW(monitor, &info) == 0) {
    return TRUE;  // skip this one, keep enumerating
  }

  core::Display display;
  // rcWork, not rcMonitor: the taskbar is not somewhere a window may be put
  // back.
  // LONG to int, said out loud: they are the same width on every Windows this
  // builds for, and an implicit narrowing inside a braced initialiser is an
  // error rather than a warning.
  display.work_area =
      core::Rect{static_cast<int>(info.rcWork.left), static_cast<int>(info.rcWork.top),
                 static_cast<int>(info.rcWork.right - info.rcWork.left),
                 static_cast<int>(info.rcWork.bottom - info.rcWork.top)};
  display.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
  display.scale = ScaleOf(monitor);

  reinterpret_cast<std::vector<core::Display>*>(data)->push_back(display);
  return TRUE;
}

}  // namespace

std::vector<core::Display> Displays() {
  std::vector<core::Display> displays;
  ::EnumDisplayMonitors(nullptr, nullptr, &OnMonitor, reinterpret_cast<LPARAM>(&displays));
  return displays;
}

}  // namespace sonora::platform
