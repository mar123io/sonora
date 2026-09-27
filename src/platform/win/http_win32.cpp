#include "sonora/platform/http.h"

#include <windows.h>
#include <winhttp.h>

#include <string>
#include <vector>

namespace sonora::platform {
namespace {

// WinHTTP rather than WinINet or libcurl. WinINet is documented as unsupported from a
// service and carries the user's Internet Explorer settings, which is a surprising
// amount of behaviour for one GET; libcurl would be a vendored dependency and a TLS
// story of its own. WinHTTP is in the operating system, uses the system certificate
// store, and has a proxy configuration a managed machine already knows about.
class Closer {
 public:
  explicit Closer(HINTERNET handle) : handle_(handle) {}
  ~Closer() {
    if (handle_ != nullptr) {
      ::WinHttpCloseHandle(handle_);
    }
  }
  Closer(const Closer&) = delete;
  Closer& operator=(const Closer&) = delete;
  [[nodiscard]] HINTERNET get() const noexcept { return handle_; }
  [[nodiscard]] bool ok() const noexcept { return handle_ != nullptr; }

 private:
  HINTERNET handle_;
};

std::wstring Widen(std::string_view text) {
  if (text.empty()) {
    return {};
  }
  const int needed =
      ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  if (needed <= 0) {
    return {};
  }
  std::wstring wide(static_cast<std::size_t>(needed), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(),
                        needed);
  return wide;
}

class WinHttpFetcher final : public sonora::update::Fetcher {
 public:
  std::optional<std::vector<std::uint8_t>> Get(std::string_view url,
                                               std::uint64_t max_bytes) override {
    const std::wstring wide_url = Widen(url);
    if (wide_url.empty()) {
      return std::nullopt;
    }

    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    std::wstring host(256, L'\0');
    std::wstring path(2048, L'\0');
    parts.lpszHostName = host.data();
    parts.dwHostNameLength = static_cast<DWORD>(host.size());
    parts.lpszUrlPath = path.data();
    parts.dwUrlPathLength = static_cast<DWORD>(path.size());
    if (::WinHttpCrackUrl(wide_url.c_str(), 0, 0, &parts) == FALSE) {
      return std::nullopt;
    }
    // https and nothing else. The manifest parser refuses any other scheme too; this is
    // the same rule at the other end, because a url that got here by some route the
    // parser did not see would otherwise be honoured.
    if (parts.nScheme != INTERNET_SCHEME_HTTPS) {
      return std::nullopt;
    }
    host.resize(parts.dwHostNameLength);
    path.resize(parts.dwUrlPathLength);

    const Closer session(::WinHttpOpen(L"Sonora-Updater/1.0",
                                       WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.ok()) {
      return std::nullopt;
    }
    // Thirty seconds each. An updater that hangs is an updater that holds a process open
    // on somebody's laptop for an afternoon.
    ::WinHttpSetTimeouts(session.get(), 30000, 30000, 30000, 30000);

    const Closer connection(::WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0));
    if (!connection.ok()) {
      return std::nullopt;
    }
    const Closer request(::WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr,
                                              WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              WINHTTP_FLAG_SECURE));
    if (!request.ok()) {
      return std::nullopt;
    }
    if (::WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                             WINHTTP_NO_REQUEST_DATA, 0, 0, 0) == FALSE ||
        ::WinHttpReceiveResponse(request.get(), nullptr) == FALSE) {
      return std::nullopt;
    }

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (::WinHttpQueryHeaders(request.get(),
                              WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                              WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                              WINHTTP_NO_HEADER_INDEX) == FALSE ||
        status != 200) {
      return std::nullopt;
    }

    std::vector<std::uint8_t> body;
    std::vector<std::uint8_t> chunk(64 * 1024);
    while (true) {
      DWORD available = 0;
      if (::WinHttpQueryDataAvailable(request.get(), &available) == FALSE) {
        return std::nullopt;
      }
      if (available == 0) {
        break;
      }
      const DWORD want =
          available < chunk.size() ? available : static_cast<DWORD>(chunk.size());
      DWORD read = 0;
      if (::WinHttpReadData(request.get(), chunk.data(), want, &read) == FALSE) {
        return std::nullopt;
      }
      if (read == 0) {
        break;
      }
      // The cap is checked as the body grows, not after: the point of it is to refuse an
      // allocation, and a check at the end would have already made it. A response longer
      // than the manifest says is not truncated to fit -- it is not the artefact.
      if (body.size() + read > max_bytes) {
        return std::nullopt;
      }
      body.insert(body.end(), chunk.begin(), chunk.begin() + read);
    }
    return body;
  }
};

}  // namespace

std::unique_ptr<sonora::update::Fetcher> MakeHttpFetcher() {
  return std::make_unique<WinHttpFetcher>();
}

}  // namespace sonora::platform
