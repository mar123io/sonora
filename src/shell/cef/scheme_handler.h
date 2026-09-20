#pragma once

#include "include/cef_scheme.h"

namespace sonora::assets {
class AssetStore;
}  // namespace sonora::assets

namespace sonora::shell {

inline constexpr char kSonoraScheme[] = "sonora";
inline constexpr char kSonoraAppHost[] = "app";
inline constexpr char kSonoraAppOrigin[] = "sonora://app";

// Called from CefApp::OnRegisterCustomSchemes, in EVERY process. A scheme
// registered only in the browser process is not recognised by the renderer,
// which shows up much later as inexplicable CORS and origin failures.
void RegisterSonoraScheme(CefRawPtr<CefSchemeRegistrar> registrar);

// Called from OnContextInitialized, browser process only. `store` must outlive
// the CEF context.
void RegisterSonoraSchemeHandlerFactory(const assets::AssetStore* store);

}  // namespace sonora::shell
