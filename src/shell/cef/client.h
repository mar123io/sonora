#pragma once

#include <memory>

#include "include/cef_client.h"

namespace sonora::bridge {
class BridgeHandlers;
class CapabilityRegistry;
}  // namespace sonora::bridge

namespace sonora::shell {

class BridgeRouter;
class EventChannel;
class ShellMetrics;

// The browser-side handler set. One object implements every handler CEF asks
// for; CefClient is a collection of factories rather than a base class with
// behaviour, so this is the conventional shape.
class SonoraClient final : public CefClient,
                           public CefLifeSpanHandler,
                           public CefLoadHandler,
                           public CefKeyboardHandler,
                           public CefRequestHandler {
 public:
  struct Options {
    bool enable_devtools = false;
    // All four must outlive the client. Owned by the runtime.
    bridge::BridgeHandlers* handlers = nullptr;
    const bridge::CapabilityRegistry* capabilities = nullptr;
    ShellMetrics* metrics = nullptr;
    // The client is what knows when a browser exists, so it is what tells the
    // event channel where to send and when to stop.
    EventChannel* events = nullptr;
  };

  explicit SonoraClient(Options options);
  ~SonoraClient() override;

  SonoraClient(const SonoraClient&) = delete;
  SonoraClient& operator=(const SonoraClient&) = delete;

  // CefClient
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
  CefRefPtr<CefKeyboardHandler> GetKeyboardHandler() override { return this; }
  CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }
  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefFrame> frame,
                                CefProcessId source_process,
                                CefRefPtr<CefProcessMessage> message) override;

  // CefLifeSpanHandler
  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
  bool DoClose(CefRefPtr<CefBrowser> browser) override;
  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;

  // CefLoadHandler
  void OnLoadError(CefRefPtr<CefBrowser> browser,
                   CefRefPtr<CefFrame> frame,
                   ErrorCode error_code,
                   const CefString& error_text,
                   const CefString& failed_url) override;

  // CefKeyboardHandler
  bool OnPreKeyEvent(CefRefPtr<CefBrowser> browser,
                     const CefKeyEvent& event,
                     CefEventHandle os_event,
                     bool* is_keyboard_shortcut) override;

  // CefRequestHandler -- implemented only to keep the bridge router informed
  // about navigation and renderer death, both of which strand pending queries.
  bool OnBeforeBrowse(CefRefPtr<CefBrowser> browser,
                      CefRefPtr<CefFrame> frame,
                      CefRefPtr<CefRequest> request,
                      bool user_gesture,
                      bool is_redirect) override;
  void OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser,
                                 TerminationStatus status,
                                 int error_code,
                                 const CefString& error_string) override;

  // Native handle of the browser view, for the host to keep sized. Null until
  // the browser exists.
  [[nodiscard]] void* BrowserViewHandle() const;

  // Asks the browser to close, and reports whether the host window should
  // cancel its own close and wait.
  //
  // Returns true  -> the request is in flight; leave the window open.
  // Returns false -> nothing left to wait for; close the window now.
  bool RequestClose();

 private:
  Options options_;
  CefRefPtr<CefBrowser> browser_;
  std::unique_ptr<BridgeRouter> router_;

  // Set by DoClose. See the comment there -- this one flag is the difference
  // between a window that closes and a window that cannot be closed at all.
  bool closing_ = false;

  IMPLEMENT_REFCOUNTING(SonoraClient);
};

}  // namespace sonora::shell
