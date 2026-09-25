#include "cef/runtime.h"

#include <sonora/platform/app_main.h>
#include <sonora/platform/event_loop.h>

#include <cstdio>
#include <memory>
#include <string>
#include <utility>

#include <sonora/bridge/capabilities.h>

#include "cef/app.h"
#include "cef/client.h"
#include "cef/event_channel.h"
#include "cef/handlers.h"
#include "cef/player_host.h"
#include "cef/shell_metrics.h"
#include "cef/timer.h"
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
std::unique_ptr<ShellHandlers> g_handlers;
CefRefPtr<ShellTimer> g_heartbeat;
bool g_initialized = false;

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
  if (g_player->Start()) {
    std::printf("audio: %s\n", g_player->description().c_str());
  } else {
    g_capabilities->Disable("player", "no audio device could be opened");
  }

  g_handlers =
      std::make_unique<ShellHandlers>(*g_capabilities, *g_metrics, *g_events, *g_player);

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

  g_player->StartStateEvents(*g_events);

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
  // Before CefShutdown: the state timer posts CEF tasks, and the device
  // callback borrows the player.
  if (g_player) {
    g_player->Stop();
  }
  platform::SetWorkCallback(nullptr);
  g_app = nullptr;
  CefShutdown();
  g_handlers.reset();
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

bool RequestBrowserClose() {
  if (!g_app) {
    return false;
  }
  CefRefPtr<SonoraClient> client = g_app->client();
  return client ? client->RequestClose() : false;
}

}  // namespace sonora::shell
