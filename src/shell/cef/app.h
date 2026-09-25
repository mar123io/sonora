#pragma once

#include <string>

#include "include/cef_app.h"

namespace sonora::assets {
class AssetStore;
}  // namespace sonora::assets

namespace sonora::bridge {
class BridgeHandlers;
class CapabilityRegistry;
}  // namespace sonora::bridge

namespace sonora::shell {

class EventChannel;
class ShellMetrics;
class LibraryHost;
class SonoraClient;
class SonoraRenderProcessHandler;

// One CefApp shared by every process.
//
// Two things have to be right in every process, not just the browser:
// OnRegisterCustomSchemes, or the renderer does not recognise sonora://, and
// GetRenderProcessHandler, or the page never gets the bridge function injected.
// Both failures are silent, which is why the helper executable builds this same
// class rather than something smaller.
class SonoraApp final : public CefApp, public CefBrowserProcessHandler {
 public:
  struct Options {
    void* parent_window = nullptr;  // native handle that hosts the browser view
    int initial_width_px = 0;
    int initial_height_px = 0;
    std::string start_url;
    bool enable_devtools = false;
    const assets::AssetStore* asset_store = nullptr;
    // Serves cover art alongside the assets, from the same origin. Null on a
    // build or a run where the index could not be opened.
    const LibraryHost* library = nullptr;
    bridge::BridgeHandlers* bridge_handlers = nullptr;
    const bridge::CapabilityRegistry* capabilities = nullptr;
    ShellMetrics* metrics = nullptr;
    EventChannel* events = nullptr;
  };

  // Browser process.
  explicit SonoraApp(Options options);
  // Child process: scheme registration and the renderer half of the bridge.
  SonoraApp();

  // Declared here and defined in the .cpp on purpose. The CefRefPtr members
  // below hold forward-declared types, and CefRefPtr's destructor calls
  // Release() on them, which needs the complete type. With an implicit
  // destructor that instantiation happens in whichever translation unit
  // includes this header -- and compiles or not depending on what else that
  // file happened to include. Out of line, it happens once, where the types
  // are complete.
  ~SonoraApp() override;

  SonoraApp(const SonoraApp&) = delete;
  SonoraApp& operator=(const SonoraApp&) = delete;

  // CefApp
  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override;
  CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override;
  void OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) override;
  void OnBeforeCommandLineProcessing(const CefString& process_type,
                                     CefRefPtr<CefCommandLine> command_line) override;

  // CefBrowserProcessHandler
  void OnContextInitialized() override;
  void OnScheduleMessagePumpWork(int64_t delay_ms) override;

  [[nodiscard]] CefRefPtr<SonoraClient> client() const { return client_; }

 private:
  Options options_;
  bool is_browser_process_ = false;
  CefRefPtr<SonoraClient> client_;
  CefRefPtr<SonoraRenderProcessHandler> render_handler_;

  IMPLEMENT_REFCOUNTING(SonoraApp);
};

}  // namespace sonora::shell
