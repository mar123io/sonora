#include "cef/scheme_handler.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <string>

#include <sonora/assets/asset_store.h>
#include "include/cef_parser.h"
#include "include/cef_resource_handler.h"
#include "include/wrapper/cef_helpers.h"

namespace sonora::shell {
namespace {

// The UI is trusted code we shipped, so the policy can be strict: no remote
// origins at all. Anything the app needs from the network goes through the
// native layer, which is the whole point of having one.
constexpr char kContentSecurityPolicy[] =
    "default-src 'self'; "
    "script-src 'self'; "
    "style-src 'self' 'unsafe-inline'; "
    "img-src 'self' data:; "
    "font-src 'self' data:; "
    "connect-src 'self'; "
    "object-src 'none'; "
    "frame-ancestors 'none'; "
    "base-uri 'none'";

constexpr char kNotFoundBody[] = "Not found.";

std::string PathFromUrl(const CefString& url) {
  CefURLParts parts;
  if (!CefParseURL(url, parts)) {
    return "/";
  }
  std::string path = CefString(&parts.path).ToString();
  if (path.empty()) {
    path = "/";
  }
  if (path == "/") {
    path = "/index.html";
  }
  return path;
}

class AssetResourceHandler final : public CefResourceHandler {
 public:
  explicit AssetResourceHandler(const assets::AssetStore* store) : store_(store) {}

  bool Open(CefRefPtr<CefRequest> request,
            bool& handle_request,
            CefRefPtr<CefCallback> callback) override {
    (void)callback;
    // Everything is resolved synchronously: embedded assets are already in
    // memory, and the development store reads a small file. Setting
    // handle_request tells CEF not to wait for the callback.
    handle_request = true;

    const std::string path = PathFromUrl(request->GetURL());
    asset_ = store_->Lookup(path);

    // Single-page-app fallback: an unknown path that is not a file request
    // serves the shell, so client-side routing works on a deep link.
    if (!asset_ && path.find('.') == std::string::npos) {
      asset_ = store_->Lookup("/index.html");
    }
    return true;
  }

  void GetResponseHeaders(CefRefPtr<CefResponse> response,
                          int64_t& response_length,
                          CefString& redirectUrl) override {
    (void)redirectUrl;

    CefResponse::HeaderMap headers;
    headers.insert({"Content-Security-Policy", kContentSecurityPolicy});
    headers.insert({"X-Content-Type-Options", "nosniff"});
    response->SetHeaderMap(headers);

    if (asset_) {
      response->SetStatus(200);
      response->SetMimeType(asset_->mime_type);
      response_length = static_cast<int64_t>(asset_->data.size());
    } else {
      response->SetStatus(404);
      response->SetMimeType("text/plain");
      response_length = static_cast<int64_t>(std::strlen(kNotFoundBody));
    }
  }

  bool Read(void* data_out,
            int bytes_to_read,
            int& bytes_read,
            CefRefPtr<CefResourceReadCallback> callback) override {
    (void)callback;

    const std::string_view body =
        asset_ ? asset_->data : std::string_view(kNotFoundBody, std::strlen(kNotFoundBody));

    if (offset_ >= body.size() || bytes_to_read <= 0) {
      bytes_read = 0;
      return false;  // complete
    }

    const std::size_t remaining = body.size() - offset_;
    const std::size_t count = std::min(static_cast<std::size_t>(bytes_to_read), remaining);
    std::memcpy(data_out, body.data() + offset_, count);
    offset_ += count;
    bytes_read = static_cast<int>(count);
    return true;
  }

  void Cancel() override {}

 private:
  const assets::AssetStore* store_;
  std::optional<assets::Asset> asset_;
  std::size_t offset_ = 0;

  IMPLEMENT_REFCOUNTING(AssetResourceHandler);
};

class SonoraSchemeHandlerFactory final : public CefSchemeHandlerFactory {
 public:
  explicit SonoraSchemeHandlerFactory(const assets::AssetStore* store) : store_(store) {}

  CefRefPtr<CefResourceHandler> Create(CefRefPtr<CefBrowser> browser,
                                       CefRefPtr<CefFrame> frame,
                                       const CefString& scheme_name,
                                       CefRefPtr<CefRequest> request) override {
    (void)browser;
    (void)frame;
    (void)scheme_name;
    (void)request;
    return new AssetResourceHandler(store_);
  }

 private:
  const assets::AssetStore* store_;

  IMPLEMENT_REFCOUNTING(SonoraSchemeHandlerFactory);
};

}  // namespace

void RegisterSonoraScheme(CefRawPtr<CefSchemeRegistrar> registrar) {
  // STANDARD gives the scheme a real origin (so it has localStorage, workers
  // and a meaningful same-origin policy); SECURE makes Chromium treat it as a
  // trustworthy origin, which most modern web APIs require; CORS_ENABLED and
  // FETCH_ENABLED let the page fetch its own assets.
  registrar->AddCustomScheme(
      kSonoraScheme, CEF_SCHEME_OPTION_STANDARD | CEF_SCHEME_OPTION_SECURE |
                         CEF_SCHEME_OPTION_CORS_ENABLED | CEF_SCHEME_OPTION_FETCH_ENABLED);
}

void RegisterSonoraSchemeHandlerFactory(const assets::AssetStore* store) {
  CefRegisterSchemeHandlerFactory(kSonoraScheme, kSonoraAppHost,
                                  new SonoraSchemeHandlerFactory(store));
}

}  // namespace sonora::shell
