#include <cstdio>

#include <sonora/core/playback_state.h>
#include <sonora/core/version.h>
#include <sonora/platform/event_loop.h>
#include <sonora/platform/window.h>

namespace {

int RunApplication() {
  std::printf("Sonora %s (%s)\n", sonora::core::kVersion, sonora::core::kGitDescribe);

  sonora::platform::WindowDesc desc;
  desc.title = "Sonora";

  auto window = sonora::platform::CreateAppWindow(desc);
  if (window == nullptr) {
    std::fprintf(stderr, "sonora: failed to create the main window\n");
    return 1;
  }

  // Closing the window ends the application for now. Week 6 puts the playback
  // engine behind this decision (keep playing in the tray, or stop cleanly),
  // and week 11 adds "apply a pending update on the way out".
  window->SetOnClose([&window] {
    window->Close();
    sonora::platform::RequestQuit(0);
  });

  window->SetOnResize([](int width_px, int height_px) {
    // Week 2: resize the CEF browser view to match.
    std::printf("resize: %dx%d px\n", width_px, height_px);
  });

  window->Show();
  return sonora::platform::RunEventLoop();
}

}  // namespace

int main() {
  return RunApplication();
}

#if defined(_WIN32)
// clang-format off
#include <windows.h>
// clang-format on

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
  return RunApplication();
}
#endif
