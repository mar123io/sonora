#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <sonora/core/deep_link.h>
#include <sonora/core/version.h>
#include <sonora/core/window_placement.h>
#include <sonora/platform/app_main.h>
#include <sonora/platform/child_window.h>
#include <sonora/platform/displays.h>
#include <sonora/platform/event_loop.h>
#include <sonora/platform/paths.h>
#include <sonora/platform/single_instance.h>
#include <sonora/platform/url_scheme.h>
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

// Where the window was last time: one line of text, next to the two databases
// and in neither of them.
//
// Not a design preference -- an ordering one. The window is created before CEF
// starts and before either store is opened, so a preference needed at that
// moment cannot live in a database that is not open yet. See the comment on
// core::SerializePlacement.
[[nodiscard]] std::optional<sonora::core::SavedPlacement> ReadPlacement(
    const std::filesystem::path& path) {
  std::ifstream file(path);
  if (!file) {
    return std::nullopt;
  }
  std::string line;
  std::getline(file, line);
  return sonora::core::ParsePlacement(line);
}

void WritePlacement(const std::filesystem::path& path,
                    const sonora::core::SavedPlacement& saved) {
  if (saved.bounds.empty()) {
    return;  // a window that no longer exists has nothing to say about where it was
  }
  std::error_code ignored;
  std::filesystem::create_directories(path.parent_path(), ignored);
  std::ofstream file(path, std::ios::trunc);
  if (!file) {
    return;
  }
  file << sonora::core::SerializePlacement(saved) << '\n';
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

  // Exactly one Sonora per session, and it is claimed here: after --play, which
  // is a different program in the same executable and has no business being
  // refused, and before anything expensive is built.
  //
  // The callback may arrive at any moment from now on, including while the rest
  // of this function is still running. ActivateWithArguments is written for
  // that: before the shell exists it does nothing, which is the right answer
  // for an activation with nowhere to go.
  auto instance = ClaimSingleInstance(kAppName, [](const std::vector<std::string>& forwarded) {
    sonora::shell::ActivateWithArguments(forwarded);
  });

  if (!instance->IsPrimary()) {
    if (instance->ForwardToPrimary(arguments)) {
      if (arguments.empty()) {
        std::printf("sonora: already running, brought it to the front\n");
      } else {
        std::printf("sonora: already running, handed %zu argument(s) over\n", arguments.size());
      }
      return 0;
    }
    // The claim is held by a process that is alive -- a dead one releases it --
    // and that process is not answering. Carrying on would mean a second CEF
    // against a user data directory the first copy has locked, and what that
    // produces is not a second Sonora: Chromium finds the lock, decides it is
    // being asked to open a tab in an existing session, and puts a stray
    // browser window on the screen. Stopping here says what happened instead.
    std::fprintf(stderr,
                 "sonora: another copy holds the claim and is not answering.\n"
                 "        Close it (or end Sonora.exe in Task Manager) and try again.\n");
    return 1;
  }

  // Every start, not once at install time: during development this executable
  // moves with every configuration, and a registration pointing at a build from
  // three weeks ago looks exactly like a link that does nothing.
  const SchemeRegistration registered =
      RegisterUrlScheme(std::string(sonora::core::kUrlScheme), kAppName);
  std::printf("scheme: %s:// %s\n", std::string(sonora::core::kUrlScheme).c_str(),
              ToString(registered).c_str());

  auto asset_store = sonora::assets::MakeDefaultAssetStore();
  std::printf("assets: %s\n", asset_store->Describe().c_str());
  if (kEnableDevTools) {
    std::printf("devtools: http://localhost:%d\n", kRemoteDebuggingPort);
  }

  const std::filesystem::path user_data = UserDataDirectory(kAppName);
  const std::filesystem::path placement_path = user_data / "window.txt";

  WindowDesc desc;
  // Decided before the window exists, because moving a window after it is on
  // screen is a visible jump and, across displays of different scale, two
  // rounds of DPI correction.
  core::PlacementRequest request;
  request.saved = ReadPlacement(placement_path);
  request.default_width = desc.width_dip;
  request.default_height = desc.height_dip;
  request.min_width = desc.min_width_dip;
  request.min_height = desc.min_height_dip;
  const std::vector<core::Display> displays = Displays();
  if (request.saved.has_value()) {
    desc.placement = core::ResolvePlacement(request, displays);
    std::printf("window: %d displays, restoring %dx%d at (%d, %d)%s\n",
                static_cast<int>(displays.size()), desc.placement->bounds.width,
                desc.placement->bounds.height, desc.placement->bounds.x,
                desc.placement->bounds.y, desc.placement->maximized ? ", maximized" : "");
  } else {
    // No saved placement: let the platform place a new window its own way
    // rather than centring it ourselves. The first run is the one case where
    // the operating system's idea of where a window goes is better than ours,
    // because it knows about the windows that are already there.
    std::printf("window: %d displays, first run\n", static_cast<int>(displays.size()));
  }

  auto window = CreateAppWindow(desc);
  if (window == nullptr) {
    std::fprintf(stderr, "sonora: failed to create the main window\n");
    return 1;
  }

  // From the window that exists, not from the size that was asked for. The two
  // are the same thing on a first run and different the moment a remembered
  // placement is restored, and the browser view is created once, at this size,
  // and resized only when the window changes size afterwards -- so a window
  // that opens at its final size never sends the correction.
  const SizePx client = window->client_size();

  sonora::shell::RuntimeConfig config;
  config.parent_window = window->native_handle();
  config.initial_width_px = client.width;
  config.initial_height_px = client.height;
  config.helper_path = ExecutablePath().parent_path() / SONORA_HELPER_NAME;
  config.user_data_dir = user_data;
  config.start_url = kStartUrl;
  config.enable_devtools = kEnableDevTools;
  config.remote_debugging_port = kEnableDevTools ? kRemoteDebuggingPort : 0;
  config.asset_store = asset_store.get();
  config.library_path = config.user_data_dir / "library.sqlite";
  config.library_root = library_root;
  config.state_path = config.user_data_dir / "state.sqlite";
  config.arguments = arguments;
  config.raise_window = [&window] {
    if (window != nullptr) {
      window->Raise();
    }
  };
  // The same two-pass close the window's own handler performs, because a quit
  // from the tray has to run the page's beforeunload exactly like clicking the
  // X does.
  config.quit = [&window] {
    if (!sonora::shell::RequestBrowserClose() && window != nullptr) {
      window->Close();
    }
  };
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

  // Captured while the window still exists. Asking for it after the loop has
  // returned would be asking a window that has already been destroyed, and the
  // answer -- an empty rectangle -- would overwrite a perfectly good saved
  // position with one that parses as "first run".
  core::SavedPlacement last_placement;
  bool have_placement = false;

  window->SetOnClose([&window, &last_placement, &have_placement] {
    if (window != nullptr) {
      last_placement = window->SavedPlacement();
      have_placement = true;
    }
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

  // The ordinary path captured this in the close handler. This is the other
  // one: a quit that never went through WM_CLOSE at all.
  if (!have_placement && window != nullptr) {
    last_placement = window->SavedPlacement();
    have_placement = !last_placement.bounds.empty();
  }
  if (have_placement) {
    WritePlacement(placement_path, last_placement);
  }

  // Order matters: the window (and with it the browser's parent) must outlive
  // CefShutdown, so the browser is destroyed before its host disappears.
  sonora::shell::StopCef();
  window.reset();
  return exit_code;
}
