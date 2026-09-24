#include "cef/bridge_router.h"

#include <bridge_generated.h>
#include <sonora/bridge/capabilities.h>
#include <sonora/bridge/protocol.h>

#include "cef/shell_metrics.h"
#include "include/wrapper/cef_helpers.h"

namespace sonora::shell {
namespace {

// Not "cefQuery". The page should not look like it is talking to a framework;
// it is talking to this application, and the name is part of the contract the
// generated TypeScript depends on.
constexpr char kQueryFunction[] = "sonoraQuery";
constexpr char kCancelFunction[] = "sonoraQueryCancel";

}  // namespace

CefMessageRouterConfig BridgeRouterConfig() {
  CefMessageRouterConfig config;
  config.js_query_function = kQueryFunction;
  config.js_cancel_function = kCancelFunction;
  return config;
}

class BridgeRouter::QueryHandler final : public CefMessageRouterBrowserSide::Handler {
 public:
  QueryHandler(bridge::BridgeHandlers& handlers,
               const bridge::CapabilityRegistry& capabilities,
               ShellMetrics& metrics)
      : handlers_(handlers), capabilities_(capabilities), metrics_(metrics) {}

  // The base declares two OnQuery overloads, one string and one binary. We
  // override only the string one; without this the other would be hidden.
  using CefMessageRouterBrowserSide::Handler::OnQuery;

  bool OnQuery(CefRefPtr<CefBrowser> browser,
               CefRefPtr<CefFrame> frame,
               int64_t query_id,
               const CefString& request,
               bool persistent,
               CefRefPtr<Callback> callback) override {
    CEF_REQUIRE_UI_THREAD();
    (void)browser;
    (void)frame;
    (void)query_id;

    if (persistent) {
      // A persistent query is one request with many answers, which sounds like
      // the way to push events and is not. It ties every event to a call the
      // page made and to the frame that made it, so a reload silently ends the
      // stream, and it gives the native side no way to speak first. Events go
      // through EventChannel instead, and this stays refused.
      callback->Failure(static_cast<int>(bridge::ErrorCode::kUnavailable),
                        "persistent queries are not supported; events are pushed, not polled");
      return true;
    }

    metrics_.NoteQuery();

    // Dispatch is documented never to throw, which matters here: this runs
    // inside a CEF callback with no way to report an escaped exception.
    const bridge::Response response =
        bridge::Dispatch(handlers_, capabilities_, request.ToString());
    if (response.ok) {
      callback->Success(response.payload);
    } else {
      callback->Failure(response.failure_code(), response.message);
    }

    // true means "this handler owns the query". Returning false with no other
    // handler registered fails the query with -1, which tells the page nothing.
    return true;
  }

 private:
  bridge::BridgeHandlers& handlers_;
  const bridge::CapabilityRegistry& capabilities_;
  ShellMetrics& metrics_;
};

BridgeRouter::BridgeRouter(bridge::BridgeHandlers& handlers,
                           const bridge::CapabilityRegistry& capabilities,
                           ShellMetrics& metrics)
    : query_handler_(std::make_unique<QueryHandler>(handlers, capabilities, metrics)),
      router_(CefMessageRouterBrowserSide::Create(BridgeRouterConfig())) {
  router_->AddHandler(query_handler_.get(), /*first=*/false);
}

BridgeRouter::~BridgeRouter() {
  // The handler is a plain object we own, and the router holds a raw pointer
  // to it. Removing it before it is destroyed is not optional.
  router_->RemoveHandler(query_handler_.get());
}

bool BridgeRouter::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                            CefRefPtr<CefFrame> frame,
                                            CefProcessId source_process,
                                            CefRefPtr<CefProcessMessage> message) {
  return router_->OnProcessMessageReceived(browser, frame, source_process, message);
}

void BridgeRouter::OnBeforeBrowse(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame) {
  router_->OnBeforeBrowse(browser, frame);
}

void BridgeRouter::OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser) {
  router_->OnRenderProcessTerminated(browser);
}

void BridgeRouter::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
  router_->OnBeforeClose(browser);
}

}  // namespace sonora::shell
