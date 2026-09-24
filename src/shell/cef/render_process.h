#pragma once

#include "include/cef_render_process_handler.h"
#include "include/wrapper/cef_message_router.h"

namespace sonora::shell {

// Renderer side of the bridge. This object lives in the helper process, not in
// the application: it is what injects window.sonoraQuery into each JavaScript
// context and relays answers back.
//
// The pairing is the part worth remembering. The browser side and this side are
// two halves of one router, created from the same configuration, and the helper
// executable has to build its half or the page's calls go nowhere -- with no
// error, because from JavaScript's point of view the function simply does not
// exist.
class SonoraRenderProcessHandler final : public CefRenderProcessHandler {
 public:
  SonoraRenderProcessHandler() = default;

  SonoraRenderProcessHandler(const SonoraRenderProcessHandler&) = delete;
  SonoraRenderProcessHandler& operator=(const SonoraRenderProcessHandler&) = delete;

  void OnWebKitInitialized() override;
  void OnContextCreated(CefRefPtr<CefBrowser> browser,
                        CefRefPtr<CefFrame> frame,
                        CefRefPtr<CefV8Context> context) override;
  void OnContextReleased(CefRefPtr<CefBrowser> browser,
                         CefRefPtr<CefFrame> frame,
                         CefRefPtr<CefV8Context> context) override;
  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefFrame> frame,
                                CefProcessId source_process,
                                CefRefPtr<CefProcessMessage> message) override;

 private:
  CefRefPtr<CefMessageRouterRendererSide> router_;

  IMPLEMENT_REFCOUNTING(SonoraRenderProcessHandler);
};

}  // namespace sonora::shell
