#include "cef/client.h"

#include <sonora/platform/event_loop.h>

#include <string>
#include <utility>

#include "include/cef_parser.h"
#include "include/wrapper/cef_helpers.h"

namespace sonora::shell {
namespace {

// Virtual-key code for F12. Spelled out rather than including <windows.h>,
// which has no business being in src/shell (ADR 0002).
constexpr int kVirtualKeyF12 = 0x7B;

std::string ErrorPage(const CefString& error_text, const CefString& failed_url) {
  return "<!doctype html><meta charset=\"utf-8\"><title>Sonora</title>"
         "<style>body{background:#121212;color:#e6e6e6;font:14px/1.6 system-ui;"
         "margin:0;display:grid;place-items:center;height:100vh}"
         "code{color:#9aa0a6}</style>"
         "<div><h1>The interface failed to load</h1>"
         "<p><code>" +
         failed_url.ToString() + "</code></p><p><code>" + error_text.ToString() +
         "</code></p></div>";
}

}  // namespace

SonoraClient::SonoraClient(Options options) : options_(options) {}

void SonoraClient::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
  CEF_REQUIRE_UI_THREAD();
  browser_ = std::move(browser);
}

bool SonoraClient::DoClose(CefRefPtr<CefBrowser> browser) {
  CEF_REQUIRE_UI_THREAD();
  (void)browser;

  // CEF closes a browser hosted in an application window in two passes. The
  // sequence is documented in full on DoClose() in cef_life_span_handler.h
  // ("Example 2"), and it is not optional:
  //
  //   1. the user clicks the window's close button; the host asks the browser
  //      to close and CANCELS its own close, so the window stays up
  //   2. CEF runs the page's onbeforeunload and onunload handlers
  //   3. CEF calls this method; returning false means "yes, go ahead"
  //   4. CEF then sends the close notification to the host window AGAIN
  //   5. this second attempt must be ALLOWED, not turned back into another
  //      request to the browser
  //
  // This flag is step 5. Without it steps 1 and 4 chase each other forever and
  // the window simply cannot be closed -- no error, no log line, just a close
  // button that does nothing.
  closing_ = true;
  return false;
}

void SonoraClient::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
  CEF_REQUIRE_UI_THREAD();
  (void)browser;
  browser_ = nullptr;
  platform::RequestQuit(0);
}

void SonoraClient::OnLoadError(CefRefPtr<CefBrowser> browser,
                               CefRefPtr<CefFrame> frame,
                               ErrorCode error_code,
                               const CefString& error_text,
                               const CefString& failed_url) {
  CEF_REQUIRE_UI_THREAD();
  (void)browser;

  // ERR_ABORTED is what a normal navigation-away looks like, not a failure.
  if (error_code == ERR_ABORTED || !frame->IsMain()) {
    return;
  }

  // A data: URL keeps the failure visible without needing the asset store,
  // which may be exactly what is broken.
  const std::string html = ErrorPage(error_text, failed_url);
  frame->LoadURL("data:text/html;charset=utf-8," + CefURIEncode(html, false).ToString());
}

bool SonoraClient::OnPreKeyEvent(CefRefPtr<CefBrowser> browser,
                                 const CefKeyEvent& event,
                                 CefEventHandle os_event,
                                 bool* is_keyboard_shortcut) {
  CEF_REQUIRE_UI_THREAD();
  (void)os_event;
  (void)is_keyboard_shortcut;

  if (!options_.enable_devtools) {
    return false;
  }
  if (event.type == KEYEVENT_RAWKEYDOWN && event.windows_key_code == kVirtualKeyF12) {
    // An untouched CefWindowInfo asks CEF to build the DevTools window however
    // it wants to. Describing that window ourselves is what failed twice:
    // SetAsPopup inherited Alloy style and CEF refused it outright, and forcing
    // Chrome style produced a window it could not populate. DevTools lives on
    // the Chrome UI layer and makes assumptions about its own window that a
    // client-provided one does not satisfy, so the fix is to stop providing one.
    //
    // The wrapper always passes the struct by address (see
    // libcef_dll/ctocpp/browser_host_ctocpp.cc), so "empty" here means a
    // zeroed cef_window_info_t, which is exactly the default configuration.
    browser->GetHost()->ShowDevTools(CefWindowInfo(), nullptr, CefBrowserSettings(),
                                     CefPoint());

    return true;  // handled; do not forward to the page
  }
  return false;
}

void* SonoraClient::BrowserViewHandle() const {
  if (!browser_) {
    return nullptr;
  }
  return static_cast<void*>(browser_->GetHost()->GetWindowHandle());
}

bool SonoraClient::RequestClose() {
  if (!browser_ || closing_) {
    return false;
  }
  browser_->GetHost()->CloseBrowser(/*force_close=*/false);
  return true;
}

}  // namespace sonora::shell
