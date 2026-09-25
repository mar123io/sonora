#include "cef/scheme_handler.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sonora/assets/asset_store.h>
#include <sonora/library/track.h>

#include "cef/library_host.h"
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

// A cover, straight out of the index.
//
// Why this is a URL at all: a page cannot be handed a megabyte of JPEG through
// the bridge -- it would be base64 in a JSON string, decoded in the renderer,
// held in memory and re-encoded for every list that shows it. An <img src> is
// what browsers are for, and the byte count never enters the bridge.
//
// The URL is the picture's content hash, which makes it immutable by
// construction: the bytes at /art/<hash> can never change, because different
// bytes are a different hash. So the response says so, and Chromium keeps it
// without asking again -- which is what makes scrolling an album grid free after
// the first pass.
class CoverResourceHandler final : public CefResourceHandler {
 public:
  explicit CoverResourceHandler(const LibraryHost* library) : library_(library) {}

  bool Open(CefRefPtr<CefRequest> request,
            bool& handle_request,
            CefRefPtr<CefCallback> callback) override {
    (void)callback;
    // Synchronous, on the CEF IO thread: this is a primary-key lookup in SQLite
    // against a connection that takes its own lock, measured in microseconds.
    // Doing it asynchronously would mean a task hop per image for no gain.
    handle_request = true;

    const std::string path = PathFromUrl(request->GetURL());
    const std::string hash = path.substr(std::strlen(kSonoraArtPath));

    // Hex only, and bounded. Not because a hash from the page can reach anything
    // -- it is a parameterised lookup in one table of pictures -- but because
    // anything else is not a hash this shell ever produced, and answering 404
    // without touching the database is the cheaper way to say so.
    const bool well_formed = !hash.empty() && hash.size() <= 64 &&
                             hash.find_first_not_of("0123456789abcdef") == std::string::npos;

    if (well_formed && library_ != nullptr) {
      cover_ = library_->Cover(hash);
    }
    return true;
  }

  void GetResponseHeaders(CefRefPtr<CefResponse> response,
                          int64_t& response_length,
                          CefString& redirectUrl) override {
    (void)redirectUrl;

    CefResponse::HeaderMap headers;
    headers.insert({"X-Content-Type-Options", "nosniff"});
    if (cover_) {
      // Content-addressed, so it can be cached for as long as the browser likes.
      headers.insert({"Cache-Control", "public, max-age=31536000, immutable"});
    }
    response->SetHeaderMap(headers);

    if (cover_) {
      response->SetStatus(200);
      // Sniffed from the bytes when the picture was indexed, never taken from the
      // tag -- which matters here, because the header above forbids the browser
      // from working it out for itself.
      response->SetMimeType(cover_->mime);
      response_length = static_cast<int64_t>(cover_->bytes.size());
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
        cover_ ? std::string_view(reinterpret_cast<const char*>(cover_->bytes.data()),
                                  cover_->bytes.size())
               : std::string_view(kNotFoundBody, std::strlen(kNotFoundBody));

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
  const LibraryHost* library_;
  std::optional<library::Cover> cover_;
  std::size_t offset_ = 0;

  IMPLEMENT_REFCOUNTING(CoverResourceHandler);
};

class SonoraSchemeHandlerFactory final : public CefSchemeHandlerFactory {
 public:
  SonoraSchemeHandlerFactory(const assets::AssetStore* store, const LibraryHost* library)
      : store_(store), library_(library) {}

  CefRefPtr<CefResourceHandler> Create(CefRefPtr<CefBrowser> browser,
                                       CefRefPtr<CefFrame> frame,
                                       const CefString& scheme_name,
                                       CefRefPtr<CefRequest> request) override {
    (void)browser;
    (void)frame;
    (void)scheme_name;

    // Two sources of bytes behind one origin: the assets we shipped, and the
    // pictures found in the user's own files. The split is by path, decided here
    // once, rather than by a branch inside a handler that would then have two
    // meanings for every member it holds.
    const std::string path = PathFromUrl(request->GetURL());
    if (path.compare(0, std::strlen(kSonoraArtPath), kSonoraArtPath) == 0) {
      return new CoverResourceHandler(library_);
    }
    return new AssetResourceHandler(store_);
  }

 private:
  const assets::AssetStore* store_;
  const LibraryHost* library_;

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

void RegisterSonoraSchemeHandlerFactory(const assets::AssetStore* store,
                                        const LibraryHost* library) {
  CefRegisterSchemeHandlerFactory(kSonoraScheme, kSonoraAppHost,
                                  new SonoraSchemeHandlerFactory(store, library));
}

}  // namespace sonora::shell
