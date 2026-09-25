#pragma once

#include "include/cef_scheme.h"

namespace sonora::assets {
class AssetStore;
}  // namespace sonora::assets

namespace sonora::shell {

class LibraryHost;

inline constexpr char kSonoraScheme[] = "sonora";
inline constexpr char kSonoraAppHost[] = "app";
inline constexpr char kSonoraAppOrigin[] = "sonora://app";

// Cover art lives under the app's own origin, at /art/<hash>, and not on a
// sonora://art host of its own: a second host is a second origin, and the page's
// content security policy says img-src 'self'. See LibraryHost::ArtUrl.
inline constexpr char kSonoraArtPath[] = "/art/";

// Called from CefApp::OnRegisterCustomSchemes, in EVERY process. A scheme
// registered only in the browser process is not recognised by the renderer,
// which shows up much later as inexplicable CORS and origin failures.
void RegisterSonoraScheme(CefRawPtr<CefSchemeRegistrar> registrar);

// Called from OnContextInitialized, browser process only. Both must outlive the
// CEF context; `library` may be null, and then /art/ answers 404 like any other
// unknown path.
void RegisterSonoraSchemeHandlerFactory(const assets::AssetStore* store,
                                        const LibraryHost* library);

}  // namespace sonora::shell
