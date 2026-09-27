#include "cef/runtime.h"

#include <sonora/platform/app_main.h>
#include <sonora/platform/event_loop.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

#include <sonora/bridge/capabilities.h>
#include <sonora/library/library.h>

#include "cef/app.h"
#include "cef/client.h"
#include "cef/crash_keys.h"
#include "cef/desktop.h"
#include "cef/event_channel.h"
#include "cef/handlers.h"
#include "cef/library_host.h"
#include "cef/media_session.h"
#include "cef/player_host.h"
#include "cef/shell_metrics.h"
#include "cef/timer.h"
#include "cef/update_service.h"
#include "include/cef_app.h"

namespace sonora::shell {
namespace {

// Everything below is owned here so it outlives the client that borrows it,
// and dies only after CefShutdown has torn down anything that could still call
// it. The order of declaration is the order of destruction reversed: handlers
// hold references to the three above them.
CefRefPtr<SonoraApp> g_app;
std::unique_ptr<bridge::CapabilityRegistry> g_capabilities;
std::unique_ptr<ShellMetrics> g_metrics;
std::unique_ptr<EventChannel> g_events;
std::unique_ptr<PlayerHost> g_player;
std::unique_ptr<LibraryHost> g_library;
std::unique_ptr<ShellMediaSession> g_media;
std::unique_ptr<DesktopIntegration> g_desktop;
std::unique_ptr<ShellHandlers> g_handlers;
CefRefPtr<ShellTimer> g_heartbeat;
CefRefPtr<ShellTimer> g_simulated_crash;
bool g_initialized = false;

// Long enough for the window to be on screen and the page to have finished loading, so the
// dump looks like one from a running application rather than one from a startup that never
// got there. Short enough that nobody waits for it.
constexpr std::int64_t kSimulateCrashDelayMs = 3000;

// A write through a null pointer, and deliberately not std::abort(): both produce a dump,
// but an access violation at an address nobody mapped is the shape of fault a real bug
// produces, and the stack it leaves is this function's rather than the runtime's.
//
// volatile so that a compiler which has proved this is undefined behaviour -- and every one
// of them has -- emits the store anyway instead of deleting the function.
[[noreturn]] void CrashThisProcessNow() {
  std::fflush(nullptr);  // the log line above this is the one that says it was on purpose
  volatile int* address = nullptr;
  *address = 0x50'11A;
  std::abort();  // not reached; here so the [[noreturn]] is true without the compiler guessing
}

// 20 Hz in, 4 Hz out. The ratio is the demonstration: the producer runs at the
// rate its own work happens at and the coalescer decides what the page sees,
// which is exactly the arrangement an audio callback will need in week 5.
constexpr std::int64_t kHeartbeatIntervalMs = 50;
std::int64_t g_heartbeat_sequence = 0;

std::string Utf8(const std::filesystem::path& path) {
  // path::string() uses the native narrow encoding on Windows and throws on
  // anything it cannot represent; u8string is always UTF-8, which is what
  // CefString::FromString expects.
  const auto utf8 = path.u8string();
  return std::string(utf8.begin(), utf8.end());
}

// The one sanctioned conditional in src/shell (see ADR 0003): CefMainArgs has
// a different shape per platform because CEF's own API does, not because our
// code does.
CefMainArgs MakeMainArgs() {
#if defined(_WIN32)
  return CefMainArgs(static_cast<HINSTANCE>(platform::NativeInstanceHandle()));
#else
  return CefMainArgs(0, nullptr);
#endif
}

void ApplySettings(CefSettings& settings, const RuntimeConfig& config) {
  // The sandbox needs cef_sandbox.lib, which is built against the static CRT.
  // Everything else here (vcpkg, the CEF wrapper) uses the dynamic CRT, and
  // mixing the two in one binary is a guaranteed crash. Shipping would settle
  // on one CRT across all dependencies and turn the sandbox back on; see the
  // consequences section of ADR 0003.
  settings.no_sandbox = true;

  // External pump: the native loop stays in charge and CEF asks to be called
  // back. See src/platform/iface/.../event_loop.h.
  settings.external_message_pump = true;
  settings.multi_threaded_message_loop = false;
  settings.windowless_rendering_enabled = false;

  settings.log_severity = LOGSEVERITY_WARNING;

  CefString(&settings.browser_subprocess_path) = Utf8(config.helper_path);
  CefString(&settings.root_cache_path) = Utf8(config.user_data_dir);
  CefString(&settings.log_file) = Utf8(config.user_data_dir / "sonora-cef.log");

  // Debug builds expose Chromium's remote debugging endpoint, so the page can
  // be inspected from a real browser at http://localhost:<port>. That is more
  // dependable than the embedded DevTools window, which depends on the GPU
  // path working, and it keeps the inspector alive while the native side is
  // stopped in a debugger.
  //
  // This is not the localhost server ADR 0003 rejected. That decision was
  // about where the UI's own origin comes from; this socket carries no
  // application content, and the port is 0 -- endpoint disabled -- in release.
  if (config.remote_debugging_port > 0) {
    settings.remote_debugging_port = config.remote_debugging_port;
  }
}

}  // namespace

int RunChildProcess() {
  // A child process needs the same CefApp, because custom schemes must be
  // registered identically in every process.
  CefRefPtr<SonoraApp> app = new SonoraApp();
  return CefExecuteProcess(MakeMainArgs(), app, nullptr);
}

bool StartCef(const RuntimeConfig& config) {
  SonoraApp::Options options;
  options.parent_window = config.parent_window;
  options.initial_width_px = config.initial_width_px;
  options.initial_height_px = config.initial_height_px;
  options.start_url = config.start_url;
  options.enable_devtools = config.enable_devtools;
  options.asset_store = config.asset_store;

  g_capabilities = std::make_unique<bridge::CapabilityRegistry>(
      bridge::CapabilityRegistry::FromEnvironment(bridge::kCapabilities));

  // The diagnostics are development instrumentation: the bridge's own counters, and a
  // heartbeat emitted at 20 Hz to watch the coalescer work from inside the page. Useful
  // every week of this project, and not something to put in front of somebody who wanted
  // to listen to music -- the panel is a floating box over their library.
  //
  // So a release does not offer them, and it is the same switch as DevTools rather than a
  // second idea of what a development build is. The page removes the panel when the
  // capability is absent, and the heartbeat timer below never starts.
  if (!config.enable_devtools) {
    g_capabilities->Disable("diagnostics",
                            "development instrumentation, not offered by a release build");
  }
  g_metrics = std::make_unique<ShellMetrics>();
  g_events = std::make_unique<EventChannel>();

  // The device is opened before CEF, so that by the time the page can ask
  // whether there is a transport, the answer is already known.
  //
  // A machine with no sound card is not a fatal condition: the player
  // capability is switched off through the same registry the environment
  // variable uses, and the page takes the degraded path it already has rather
  // than a second one nobody exercises.
  g_player = std::make_unique<PlayerHost>();
  const bool audio_started = g_player->Start();
  if (audio_started) {
    std::printf("audio: %s\n", g_player->description().c_str());
  } else {
    g_capabilities->Disable("player", "no audio device could be opened");
  }

  // The index, on the same terms as the device: opened before CEF so the answer
  // to "is there a library" is known before the page can ask, and switched off
  // through the same registry when it cannot be opened at all.
  g_library = std::make_unique<LibraryHost>();
  if (g_library->Start(config.library_path)) {
    std::printf("library: %s\n", g_library->description().c_str());
  } else {
    g_capabilities->Disable("library", "the library index could not be opened");
    std::fprintf(stderr, "library: %s\n", g_library->description().c_str());
  }

  g_handlers = std::make_unique<ShellHandlers>(*g_capabilities, *g_metrics, *g_events,
                                               *g_player, *g_library);

  options.library = g_library.get();
  options.bridge_handlers = g_handlers.get();
  options.capabilities = g_capabilities.get();
  options.metrics = g_metrics.get();
  options.events = g_events.get();

  g_app = new SonoraApp(std::move(options));

  // Registered before CefInitialize: CEF can ask for work during
  // initialization, and ScheduleWork needs the pump window to already exist.
  platform::SetWorkCallback([] { CefDoMessageLoopWork(); });

  CefSettings settings;
  ApplySettings(settings, config);

  if (!CefInitialize(MakeMainArgs(), settings, g_app, nullptr)) {
    g_app = nullptr;
    g_handlers.reset();
    return false;
  }
  g_initialized = true;

  // The first moment a crash key can be set: Crashpad is started inside CefInitialize, so
  // the same four calls one line higher up would be four no-ops. Nothing is lost by
  // waiting -- a fault before the handler exists produces no dump for a key to be on -- and
  // all four answers were already known before the call. See cef/crash_keys.h and ADR 0014.
  SetBuildCrashKeys();
  SetLibraryCrashKey(library::Library::kSchemaVersion);
  SetAudioCrashKey(audio_started, g_player->description());
  SetUpdateCrashKey(UpdateStage());

  g_player->StartStateEvents(*g_events);
  g_library->StartStatusEvents(*g_events);

  // The system's media panel, tied to the application window rather than to the
  // browser view: the system shows the window's application, and the browser
  // view is a child nobody outside this process should be told about.
  //
  // Started after CefInitialize because its requests are marshalled with a CEF
  // task, and refused gracefully: a machine without it plays music with a
  // keyboard that does nothing, which is worth a log line and not a failure.
  g_media = std::make_unique<ShellMediaSession>(*g_player, *g_library);
  if (g_media->Start(config.parent_window)) {
    std::printf("media: %s\n", g_media->description().c_str());
  } else {
    std::fprintf(stderr, "media: %s\n", g_media->description().c_str());
  }

  // The tray, the taskbar's thumbnail buttons and the jump list, plus the
  // durable store they are built on. After the media session because it wants
  // the same window and the same player, and because when two things fail the
  // log reads better in a fixed order.
  g_desktop = std::make_unique<DesktopIntegration>(*g_player, *g_library);
  DesktopIntegration::Callbacks callbacks;
  callbacks.raise_window = config.raise_window;
  callbacks.quit = config.quit;
  if (g_desktop->Start(config.parent_window, config.state_path, std::move(callbacks))) {
    std::printf("desktop: %s\n", g_desktop->description().c_str());
  } else {
    std::fprintf(stderr, "desktop: unavailable, no tray and no jump list\n");
    g_desktop.reset();
  }

  // A sonora:// link this process was launched with. Posted rather than run
  // here: the player is ready, but acting on it before the loop has turned
  // would mean the window is not on screen yet when it starts playing.
  if (!config.arguments.empty()) {
    ActivateWithArguments(config.arguments);
  }

  // A scan asked for on the command line starts here, after the event channel
  // exists -- otherwise its progress would be posted at a sink that is not there
  // yet and the first thing the page saw would be a finished scan.
  if (!config.library_root.empty() || config.scan_at_startup) {
    if (g_library->StartScan(config.library_root)) {
      std::printf("library: scanning %s\n", g_library->Status().root.c_str());
    }
  }

  if (g_capabilities->IsEnabled("diagnostics")) {
    // Started before the browser exists on purpose: the channel drops what it
    // cannot deliver, so there is no ordering to get right here, and one less
    // thing has to survive a browser that comes and goes.
    //
    // Note what is not written anywhere here: the string
    // "diagnostics.heartbeat". bridge::Events is generated from the schema and
    // has one method per event, so an event that does not exist is a compile
    // error rather than a message the page never receives.
    g_heartbeat = ShellTimer::Every(kHeartbeatIntervalMs, [] {
      bridge::DiagnosticsHeartbeatEvent payload;
      payload.sequence = ++g_heartbeat_sequence;
      payload.uptimeMs = g_metrics->uptime_ms();
      bridge::Events(*g_events).DiagnosticsHeartbeat(payload);
    });
  }

  if (config.simulate_crash == "browser") {
    // After everything else is started, because the point of the exercise is a dump from a
    // process that was doing its job: the audio device open, the library indexed, the
    // crash keys set. A fault three seconds into a run is a fault with a stack worth
    // reading, and it is the only way to find out whether the four keys above actually
    // arrive at the receiver.
    std::printf("crash: faulting deliberately in %d ms -- --simulate-crash=browser\n",
                static_cast<int>(kSimulateCrashDelayMs));
    g_simulated_crash = ShellTimer::Once(kSimulateCrashDelayMs, [] { CrashThisProcessNow(); });
  }

  // Gets the loop turning so OnContextInitialized fires and the browser is
  // created. Without this the application would sit idle waiting for input.
  platform::ScheduleWork(0);
  return true;
}

std::string CapabilitySummary() {
  return g_capabilities ? g_capabilities->Summary() : std::string("not started");
}

void StopCef() {
  if (!g_initialized) {
    return;
  }
  // Cancelled before CefShutdown: a heartbeat that fires during teardown would
  // reach into an EventChannel that is about to be destroyed.
  if (g_heartbeat) {
    g_heartbeat->Cancel();
    g_heartbeat = nullptr;
  }
  // The same, and it matters more: a deliberate crash that fires while the window is
  // closing would produce a dump of a teardown rather than of a run, which is the one
  // dump nobody asked for.
  if (g_simulated_crash) {
    g_simulated_crash->Cancel();
    g_simulated_crash = nullptr;
  }
  // Before CefShutdown: the state timer posts CEF tasks, and the device
  // callback borrows the player.
  if (g_player) {
    g_player->Stop();
  }
  // Also before CefShutdown, and for the same reason plus one: the scan thread
  // has to be asked to stop and joined, or closing the window would wait for a
  // scan of fifty thousand files to finish.
  if (g_library) {
    g_library->Stop();
  }
  // Before CefShutdown as well: it holds a timer that posts CEF tasks, and the
  // session should close rather than leave the panel naming an application that
  // is on its way out.
  if (g_media) {
    g_media->Stop();
  }
  // And the same again for the tray: an icon left in the notification area
  // after the process is gone is the classic Windows ghost, and it stays there
  // until somebody waves the mouse over it.
  if (g_desktop) {
    g_desktop->Stop();
  }
  platform::SetWorkCallback(nullptr);
  g_app = nullptr;
  CefShutdown();
  g_handlers.reset();
  g_desktop.reset();
  g_media.reset();
  g_library.reset();
  g_player.reset();
  g_events.reset();
  g_metrics.reset();
  g_capabilities.reset();
  g_initialized = false;
}

void* BrowserViewHandle() {
  if (!g_app) {
    return nullptr;
  }
  CefRefPtr<SonoraClient> client = g_app->client();
  return client ? client->BrowserViewHandle() : nullptr;
}

void ActivateWithArguments(const std::vector<std::string>& arguments) {
  if (g_desktop) {
    g_desktop->Activate(arguments);
  }
}

bool RequestBrowserClose() {
  if (!g_app) {
    return false;
  }
  CefRefPtr<SonoraClient> client = g_app->client();
  return client ? client->RequestClose() : false;
}

}  // namespace sonora::shell
