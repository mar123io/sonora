#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <sonora/core/version.h>
#include <sonora/platform/app_main.h>
#include <sonora/platform/child_window.h>
#include <sonora/platform/event_loop.h>
#include <sonora/platform/paths.h>
#include <sonora/platform/window.h>

#include <sonora/assets/asset_store.h>
#include "cef/runtime.h"
#include "play_command.h"

namespace {

constexpr char kAppName[] = "Sonora";
constexpr char kStartUrl[] = "sonora://app/index.html";
constexpr int kRemoteDebuggingPort = 9222;

#if defined(SONORA_ENABLE_DEVTOOLS)
constexpr bool kEnableDevTools = true;
#else
constexpr bool kEnableDevTools = false;
#endif

// The arguments arrive as UTF-8 (see platform/app_main.h). Constructing a
// std::filesystem::path from a narrow string on Windows would reinterpret it in
// the system's ANSI code page, which is the bug the conversion in the platform
// layer exists to avoid -- undoing it here would be quietly throwing that away.
std::filesystem::path Utf8Path(const std::string& utf8) {
  return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

}  // namespace

int sonora::platform::AppMain() {
  std::printf("Sonora %s (%s)\n", sonora::core::kVersion, sonora::core::kGitDescribe);

  // Checked before anything else is built. --play is a different program that
  // happens to live in the same executable: no window, no CEF, no bridge, so
  // an underrun it reports can only have come from the audio path.
  const auto& arguments = CommandLineArguments();
  std::filesystem::path library_root;
  for (std::size_t i = 0; i < arguments.size(); ++i) {
    // --library <folder>: index that folder at startup and remember it. There is
    // no folder picker yet -- that needs a native file dialog, which is week 8
    // work -- and a command-line flag is the honest stand-in rather than a text
    // box in the page that would put a path back on the bridge, which is the
    // thing week 7 just removed.
    if (arguments[i] == "--library") {
      if (i + 1 >= arguments.size()) {
        std::fprintf(stderr, "sonora: --library needs a folder\n");
        return 2;
      }
      library_root = Utf8Path(arguments[i + 1]);
      continue;
    }
    if (arguments[i] != "--play") {
      continue;
    }
    // Everything after --play is a track. More than one is how the gapless
    // claim gets checked without the interface: N files, N-1 joins, and the
    // command says how many it made.
    std::vector<std::filesystem::path> tracks;
    for (std::size_t track = i + 1; track < arguments.size(); ++track) {
      tracks.push_back(Utf8Path(arguments[track]));
    }
    if (tracks.empty()) {
      std::fprintf(stderr, "sonora: --play needs at least one file path\n");
      return 2;
    }
    return sonora::shell::RunPlayCommand(tracks);
  }

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
  config.library_path = config.user_data_dir / "library.sqlite";
  config.library_root = library_root;
  // Every start rescans whatever folder was last indexed. It is cheap -- the
  // scan compares each file's timestamp and size against the index and reads
  // nothing that has not changed -- and it means a library that is edited outside
  // Sonora is right again by the time the window has finished opening.
  config.scan_at_startup = true;

  if (!sonora::shell::StartCef(config)) {
    std::fprintf(stderr, "sonora: CEF failed to initialise\n");
    return 1;
  }

  // Printed rather than left implicit: a capability switched off by an
  // environment variable is invisible from inside the application, and the
  // first symptom is a part of the interface quietly not being there.
  std::printf("capabilities: %s\n", sonora::shell::CapabilitySummary().c_str());

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
