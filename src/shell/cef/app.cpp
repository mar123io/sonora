#include "cef/app.h"

#include <sonora/platform/event_loop.h>

#include <utility>

#include "cef/client.h"
#include "cef/scheme_handler.h"
#include "include/cef_browser.h"
#include "include/wrapper/cef_helpers.h"

namespace sonora::shell {

SonoraApp::SonoraApp(Options options)
    : options_(std::move(options)), is_browser_process_(true) {}

SonoraApp::SonoraApp() = default;

CefRefPtr<CefBrowserProcessHandler> SonoraApp::GetBrowserProcessHandler() {
  return is_browser_process_ ? this : nullptr;
}

void SonoraApp::OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) {
  RegisterSonoraScheme(registrar);
}

void SonoraApp::OnContextInitialized() {
  CEF_REQUIRE_UI_THREAD();

  RegisterSonoraSchemeHandlerFactory(options_.asset_store);

  client_ = new SonoraClient(SonoraClient::Options{options_.enable_devtools});

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
