#include "cef/render_process.h"

#include "cef/bridge_router.h"

namespace sonora::shell {

void SonoraRenderProcessHandler::OnWebKitInitialized() {
  // Created once per render process, before any context exists. Building it in
  // OnContextCreated instead would make a new router per frame and lose every
  // pending query on navigation.
  router_ = CefMessageRouterRendererSide::Create(BridgeRouterConfig());
}

void SonoraRenderProcessHandler::OnContextCreated(CefRefPtr<CefBrowser> browser,
                                                  CefRefPtr<CefFrame> frame,
                                                  CefRefPtr<CefV8Context> context) {
  if (router_) {
    // This is what puts window.sonoraQuery into the page.
    router_->OnContextCreated(browser, frame, context);
  }
}

void SonoraRenderProcessHandler::OnContextReleased(CefRefPtr<CefBrowser> browser,
                                                   CefRefPtr<CefFrame> frame,
                                                   CefRefPtr<CefV8Context> context) {
  if (router_) {
    // Cancels anything still in flight for this context, so the browser side
    // stops holding a callback into a frame that no longer exists.
    router_->OnContextReleased(browser, frame, context);
  }
}

bool SonoraRenderProcessHandler::OnProcessMessageReceived(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    CefProcessId source_process,
    CefRefPtr<CefProcessMessage> message) {
  if (router_ && router_->OnProcessMessageReceived(browser, frame, source_process, message)) {
    return true;
  }
  return false;
}

}  // namespace sonora::shell
