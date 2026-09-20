#include "cef/runtime.h"

#include <sonora/platform/app_main.h>
#include <sonora/platform/event_loop.h>

#include <string>
#include <utility>

#include "cef/app.h"
#include "cef/client.h"
#include "include/cef_app.h"

namespace sonora::shell {
namespace {

CefRefPtr<SonoraApp> g_app;
bool g_initialized = false;

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

  g_app = new SonoraApp(std::move(options));

  // Registered before CefInitialize: CEF can ask for work during
  // initialization, and ScheduleWork needs the pump window to already exist.
  platform::SetWorkCallback([] { CefDoMessageLoopWork(); });

  CefSettings settings;
  ApplySettings(settings, config);

  if (!CefInitialize(MakeMainArgs(), settings, g_app, nullptr)) {
    g_app = nullptr;
    return false;
  }
  g_initialized = true;

  // Gets the loop turning so OnContextInitialized fires and the browser is
  // created. Without this the application would sit idle waiting for input.
  platform::ScheduleWork(0);
  return true;
}

void StopCef() {
  if (!g_initialized) {
    return;
  }
  platform::SetWorkCallback(nullptr);
  g_app = nullptr;
  CefShutdown();
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
