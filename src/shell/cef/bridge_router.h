#pragma once

#include <memory>

#include "include/wrapper/cef_message_router.h"

namespace sonora::bridge {
class BridgeHandlers;
class CapabilityRegistry;
}  // namespace sonora::bridge

namespace sonora::shell {

class ShellMetrics;

// Both processes must build the router from identical configuration, so the
// names of the injected JavaScript functions are defined once, here, and used
// by the browser side and the renderer side alike.
[[nodiscard]] CefMessageRouterConfig BridgeRouterConfig();

// Browser side of the bridge.
//
// Its whole job is to move two strings: a request out of the page and into
// bridge::Dispatch, and an answer back. Everything about what the methods mean
// lives in the portable bridge target, which is why this file is short.
//
// Events go the other way and do not come through here at all; see
// cef/event_channel.h.
class BridgeRouter {
 public:
  // All three references must outlive the router.
  BridgeRouter(bridge::BridgeHandlers& handlers,
               const bridge::CapabilityRegistry& capabilities,
               ShellMetrics& metrics);
  ~BridgeRouter();

  BridgeRouter(const BridgeRouter&) = delete;
  BridgeRouter& operator=(const BridgeRouter&) = delete;

  // All four are forwarded from SonoraClient. The router's documentation is
  // explicit that it only works if every one of them arrives: the last three
  // are what cancel queries still in flight when a page navigates away, a
  // renderer dies, or a browser closes. Skipping them does not break the happy
  // path, which is exactly why it is worth being deliberate about them.
  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefFrame> frame,
                                CefProcessId source_process,
                                CefRefPtr<CefProcessMessage> message);
  void OnBeforeBrowse(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame);
  void OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser);
  void OnBeforeClose(CefRefPtr<CefBrowser> browser);

 private:
  class QueryHandler;

  std::unique_ptr<QueryHandler> query_handler_;
  CefRefPtr<CefMessageRouterBrowserSide> router_;
};

}  // namespace sonora::shell
