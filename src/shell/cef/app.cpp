#include "cef/app.h"

#include <sonora/platform/event_loop.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

#include "cef/client.h"
#include "cef/render_process.h"
#include "cef/scheme_handler.h"
#include "include/cef_browser.h"
#include "include/cef_command_line.h"
#include "include/wrapper/cef_helpers.h"

namespace sonora::shell {
namespace {

// Extra Chromium switches, from the environment, space separated, each written
// without its leading dashes: "disable-direct-composition use-angle=gl".
constexpr char kSwitchesVariable[] = "SONORA_CEF_SWITCHES";

}  // namespace

SonoraApp::SonoraApp(Options options)
    : options_(std::move(options)),
      is_browser_process_(true),
      render_handler_(new SonoraRenderProcessHandler()) {}

SonoraApp::SonoraApp() : render_handler_(new SonoraRenderProcessHandler()) {}

SonoraApp::~SonoraApp() = default;

CefRefPtr<CefBrowserProcessHandler> SonoraApp::GetBrowserProcessHandler() {
  return is_browser_process_ ? this : nullptr;
}

CefRefPtr<CefRenderProcessHandler> SonoraApp::GetRenderProcessHandler() {
  // CEF only asks for this in a render process, so returning it unconditionally
  // costs nothing and removes a way to get the wiring wrong.
  return render_handler_;
}

void SonoraApp::OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) {
  RegisterSonoraScheme(registrar);
}

void SonoraApp::OnBeforeCommandLineProcessing(const CefString& process_type,
                                              CefRefPtr<CefCommandLine> command_line) {
  // An escape hatch for the graphics stack, and the reason it exists rather
  // than a fixed list of switches:
  //
  // On one machine here, Chromium's DirectComposition presenter fails on an AMD
  // driver -- VideoProcessorGetOutputExtension returns 0x80004005, the GPU
  // process dies, and the window stays blank with no error anywhere a user
  // would look. --disable-direct-composition fixes it completely.
  //
  // The temptation is to pass that switch always. It would be wrong: DirectComposition
  // is the efficient presentation path on Windows, and degrading every machine
  // because of one driver is how software gets slow one workaround at a time.
  // Chromium already has the right mechanism for this -- a driver blocklist,
  // updated with each release -- and a project pinned to one CEF build cannot
  // update it, which is the honest reason a knob is needed at all.
  //
  // So the default is Chromium's own behaviour, and the knob is an environment
  // variable: set once, it survives every run, and it is not limited to this
  // one bug.
#if defined(_MSC_VER)
  // Same suppression, and the same reason, as CapabilityRegistry::FromEnvironment:
  // MSVC deprecates std::getenv over a race with putenv, and this program never
  // writes the environment. Narrow on purpose -- the rest of the file keeps the
  // warning.
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
  const char* extra = std::getenv(kSwitchesVariable);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
  if (extra == nullptr || *extra == '\0') {
    return;
  }

  std::string applied;
  const std::string text(extra);
  std::size_t index = 0;
  while (index < text.size()) {
    while (index < text.size() && text[index] == ' ') {
      ++index;
    }
    const std::size_t begin = index;
    while (index < text.size() && text[index] != ' ') {
      ++index;
    }
    if (begin == index) {
      break;
    }

    // Written without dashes, so nobody has to remember whether Chromium wants
    // one or two. A switch with a value is name=value.
    const std::string entry = text.substr(begin, index - begin);
    const std::size_t equals = entry.find('=');
    if (equals == std::string::npos) {
      command_line->AppendSwitch(entry);
    } else {
      command_line->AppendSwitchWithValue(entry.substr(0, equals), entry.substr(equals + 1));
    }
    applied += (applied.empty() ? "" : " ") + entry;
  }

  // Said out loud, in every process that takes them. A graphics workaround that
  // is invisible is a workaround somebody spends an afternoon rediscovering.
  if (!applied.empty()) {
    const std::string which =
        process_type.empty() ? std::string("browser") : process_type.ToString();
    std::printf("cef: %s switches from %s: %s\n", which.c_str(), kSwitchesVariable,
                applied.c_str());
  }
}

void SonoraApp::OnContextInitialized() {
  CEF_REQUIRE_UI_THREAD();

  RegisterSonoraSchemeHandlerFactory(options_.asset_store, options_.library);

  SonoraClient::Options client_options;
  client_options.enable_devtools = options_.enable_devtools;
  client_options.handlers = options_.bridge_handlers;
  client_options.capabilities = options_.capabilities;
  client_options.metrics = options_.metrics;
  client_options.events = options_.events;
  client_ = new SonoraClient(client_options);

  CefWindowInfo window_info;
  const CefRect bounds(0, 0, options_.initial_width_px, options_.initial_height_px);
  window_info.SetAsChild(static_cast<CefWindowHandle>(options_.parent_window), bounds);

  // Alloy style is the runtime that supports living inside a host-owned native
  // window. Chrome style brings its own browser window furniture, which is not
  // what a product shell wants.
  window_info.runtime_style = CEF_RUNTIME_STYLE_ALLOY;

  CefBrowserSettings browser_settings;
  // Matches the window's paint colour so there is no white flash before the
  // first frame of the page.
  browser_settings.background_color = CefColorSetARGB(255, 18, 18, 18);

  CefBrowserHost::CreateBrowser(window_info, client_, options_.start_url, browser_settings,
                                nullptr, nullptr);
}

void SonoraApp::OnScheduleMessagePumpWork(int64_t delay_ms) {
  // Called from CEF's internals, possibly off the loop thread. The platform
  // layer is responsible for getting back onto it.
  platform::ScheduleWork(delay_ms);
}

}  // namespace sonora::shell
