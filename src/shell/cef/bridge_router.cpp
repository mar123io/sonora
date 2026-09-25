#include "cef/bridge_router.h"

#include <bridge_generated.h>
#include <sonora/bridge/capabilities.h>
#include <sonora/bridge/protocol.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "cef/shell_metrics.h"
#include "include/wrapper/cef_helpers.h"

namespace sonora::shell {
namespace {

// Not "cefQuery". The page should not look like it is talking to a framework;
// it is talking to this application, and the name is part of the contract the
// generated TypeScript depends on.
constexpr char kQueryFunction[] = "sonoraQuery";
constexpr char kCancelFunction[] = "sonoraQueryCancel";

// A query that holds the UI thread this long has stopped being slow and started
// being a freeze: at 60 Hz it is fifteen dropped frames.
constexpr std::int64_t kSlowQueryMs = 250;

// SONORA_TRACE_BRIDGE=1 logs every call, before and after. Off by default
// because a transport at four updates a second would bury everything else.
[[nodiscard]] bool Tracing() {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)  // see CapabilityRegistry::FromEnvironment
#endif
  static const bool enabled = std::getenv("SONORA_TRACE_BRIDGE") != nullptr;
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
  return enabled;
}

// The method, pulled out of the envelope for the log alone.
//
// Deliberately not a JSON parse: this runs before dispatch on every single
// query, and a log line must never be able to fail a call that would otherwise
// have worked.
[[nodiscard]] std::string MethodName(const CefString& request) {
  const std::string text = request.ToString();
  const std::size_t key = text.find("\"method\"");
  if (key == std::string::npos) {
    return "(no method)";
  }
  const std::size_t open = text.find('"', text.find(':', key) + 1);
  if (open == std::string::npos) {
    return "(no method)";
  }
  const std::size_t close = text.find('"', open + 1);
  if (close == std::string::npos) {
    return "(no method)";
  }
  return text.substr(open + 1, close - open - 1);
}

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

    // Every query is served here, on the UI thread, one after another. That is
    // the right place for it -- handlers touch objects that belong to this
    // thread -- and it has a consequence worth being able to see: a handler that
    // blocks does not merely answer late, it holds up every call behind it,
    // including the ones that have nothing to do with it. From the page that
    // looks like the whole application freezing, and the timeout it reports
    // names whichever call happened to be waiting, not the one at fault.
    //
    // So the slow ones say so. The trace says which call is in flight before it
    // finishes, which is the only way to name a call that never finishes at all.
    const std::string method = MethodName(request);
    if (Tracing()) {
      std::printf("bridge: -> %s\n", method.c_str());
      std::fflush(stdout);
    }
    const auto started = std::chrono::steady_clock::now();

    // Dispatch is documented never to throw, which matters here: this runs
    // inside a CEF callback with no way to report an escaped exception.
    const bridge::Response response =
        bridge::Dispatch(handlers_, capabilities_, request.ToString());

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    if (Tracing()) {
      std::printf("bridge: <- %s (%lld ms)\n", method.c_str(), static_cast<long long>(elapsed));
      std::fflush(stdout);
    } else if (elapsed >= kSlowQueryMs) {
      // Not a trace, a fault: at this length the interface has visibly stopped.
      std::printf("bridge: %s took %lld ms on the UI thread\n", method.c_str(),
                  static_cast<long long>(elapsed));
      std::fflush(stdout);
    }

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
  // The handler is a plain object we own, and the router holds a raw pointer to
  // it, so removing it before it is destroyed is not optional -- but
  // RemoveHandler is one of CEF's UI-thread-only calls, and it CHECKs, which
  // means aborting the process rather than returning an error.
  //
  // This is where that bit: a CefClient is reference counted and CEF drops its
  // last reference *after* CefShutdown, so this destructor used to run in a
  // process whose UI thread no longer exists. The result was
  //
  //   FATAL:cef_message_router.cc(211)] Check failed: CefCurrentlyOn(TID_UI)
  //
  // on every clean exit, since week 3 -- invisible because it happens after the
  // window has gone, and found only by reading the CEF log while chasing
  // something else entirely.
  //
  // The fix is that SonoraClient::OnBeforeClose destroys this object while CEF
  // is still running and we are on the right thread. The check below is for the
  // one path where that never happens: a browser that was never created, so
  // OnBeforeClose never fired. There the router is simply released; the handler
  // it points at dies with this object in the same breath, and there are no
  // pending queries because there was never a page to make one.
  if (CefCurrentlyOn(TID_UI)) {
    router_->RemoveHandler(query_handler_.get());
  }
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
