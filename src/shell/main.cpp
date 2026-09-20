#include <cstdio>
#include <memory>

#include <sonora/core/version.h>
#include <sonora/platform/app_main.h>
#include <sonora/platform/child_window.h>
#include <sonora/platform/event_loop.h>
#include <sonora/platform/paths.h>
#include <sonora/platform/window.h>

#include <sonora/assets/asset_store.h>
#include "cef/runtime.h"

namespace {

constexpr char kAppName[] = "Sonora";
constexpr char kStartUrl[] = "sonora://app/index.html";
constexpr int kRemoteDebuggingPort = 9222;

#if defined(SONORA_ENABLE_DEVTOOLS)
constexpr bool kEnableDevTools = true;
#else
constexpr bool kEnableDevTools = false;
#endif

}  // namespace

int sonora::platform::AppMain() {
  std::printf("Sonora %s (%s)\n", sonora::core::kVersion, sonora::core::kGitDescribe);

  auto asset_store = sonora::assets::MakeDefaultAssetStore();
  std::printf("assets: %s\n", asset_store->Describe().c_str());
  if (kEnableDevTools) {
    std::printf("devtools: http://localhost:%d\n", kRemoteDebuggingPort);
  }

  WindowDesc desc;
  auto window = CreateAppWindow(desc);
  if (window == nullptr) {
    std::fprintf(stderr, "sonora: failed to create the main window\n");
    return 1;
  }

  const float scale = window->scale_factor();

  sonora::shell::RuntimeConfig config;
  config.parent_window = window->native_handle();
  config.initial_width_px = static_cast<int>(static_cast<float>(desc.width_dip) * scale);
  config.initial_height_px = static_cast<int>(static_cast<float>(desc.height_dip) * scale);
  config.helper_path = ExecutablePath().parent_path() / SONORA_HELPER_NAME;
  config.user_data_dir = UserDataDirectory(kAppName);
  config.start_url = kStartUrl;
  config.enable_devtools = kEnableDevTools;
  config.remote_debugging_port = kEnableDevTools ? kRemoteDebuggingPort : 0;
  config.asset_store = asset_store.get();

  if (!sonora::shell::StartCef(config)) {
    std::fprintf(stderr, "sonora: CEF failed to initialise\n");
    return 1;
  }

  // The browser view is a native child of the window, so the host owns its
  // geometry. CEF never resizes itself.
  window->SetOnResize([](int width_px, int height_px) {
    SetChildWindowBounds(sonora::shell::BrowserViewHandle(), 0, 0, width_px, height_px);
  });

  window->SetOnClose([&window] {
    // CEF's close is a two-pass handshake, and this handler sees both passes.
    //
    // First pass: RequestBrowserClose asks the browser to close and returns
    // true, so the window stays open while the page runs onbeforeunload.
    // Second pass: CEF has agreed, RequestBrowserClose returns false, and the
    // window is destroyed for real. See SonoraClient::DoClose.
    if (!sonora::shell::RequestBrowserClose()) {
      window->Close();
    }
  });

  window->Show();

  const int exit_code = RunEventLoop();

  // Order matters: the window (and with it the browser's parent) must outlive
  // CefShutdown, so the browser is destroyed before its host disappears.
  sonora::shell::StopCef();
  window.reset();
  return exit_code;
}
