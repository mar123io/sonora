#pragma once

#include "include/cef_client.h"

namespace sonora::shell {

// The browser-side handler set. One object implements every handler CEF asks
// for; CefClient is a collection of factories rather than a base class with
// behaviour, so this is the conventional shape.
class SonoraClient final : public CefClient,
                           public CefLifeSpanHandler,
                           public CefLoadHandler,
                           public CefKeyboardHandler {
 public:
  struct Options {
    bool enable_devtools = false;
  };

  explicit SonoraClient(Options options);

  SonoraClient(const SonoraClient&) = delete;
  SonoraClient& operator=(const SonoraClient&) = delete;

  // CefClient
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
  CefRefPtr<CefKeyboardHandler> GetKeyboardHandler() override { return this; }

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

  // Native handle of the browser view, for the host to keep sized. Null until
  // the browser exists.
  [[nodiscard]] void* BrowserViewHandle() const;

  // Asks the browser to close, and reports whether the host window should
  // cancel its own close and wait.
  //
  // Returns true  -> the request is in flight; leave the window open.
  // Returns false -> nothing left to wait for; close the window now.
  //
  // The false case covers both "there is no browser" and "CEF has already
  // agreed to the close and is asking the window to go away". Conflating them
  // is deliberate: from the window's point of view they are the same
  // instruction.
  bool RequestClose();

 private:
  Options options_;
  CefRefPtr<CefBrowser> browser_;

  // Set by DoClose. See the comment there -- this one flag is the difference
  // between a window that closes and a window that cannot be closed at all.
  bool closing_ = false;

  IMPLEMENT_REFCOUNTING(SonoraClient);
};

}  // namespace sonora::shell
