#pragma once

#include <string>

#include "include/cef_app.h"

namespace sonora::assets {
class AssetStore;
}  // namespace sonora::assets

namespace sonora::shell {

class SonoraClient;

// One CefApp shared by every process.
//
// The subtlety worth knowing: OnRegisterCustomSchemes must run in the browser
// process AND in every child process, so the helper executable constructs this
// same class. Only the browser process gets a CefBrowserProcessHandler.
class SonoraApp final : public CefApp, public CefBrowserProcessHandler {
 public:
  struct Options {
    void* parent_window = nullptr;  // native handle that hosts the browser view
    int initial_width_px = 0;
    int initial_height_px = 0;
    std::string start_url;
    bool enable_devtools = false;
    const assets::AssetStore* asset_store = nullptr;
  };

  // Browser process.
  explicit SonoraApp(Options options);
  // Child process: scheme registration only.
  SonoraApp();

  SonoraApp(const SonoraApp&) = delete;
  SonoraApp& operator=(const SonoraApp&) = delete;

  // CefApp
  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override;
  void OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) override;

  // CefBrowserProcessHandler
  void OnContextInitialized() override;
  void OnScheduleMessagePumpWork(int64_t delay_ms) override;

  [[nodiscard]] CefRefPtr<SonoraClient> client() const { return client_; }

 private:
  Options options_;
  bool is_browser_process_ = false;
  CefRefPtr<SonoraClient> client_;

  IMPLEMENT_REFCOUNTING(SonoraApp);
};

}  // namespace sonora::shell
