#include "sonora/platform/http.h"

namespace sonora::platform {

// No HTTP client on macOS and Linux, for the same reason there is no updater on them:
// see shared/update_host_none.cpp. Returning nothing rather than an implementation that
// cannot be used keeps the answer in one place.
std::unique_ptr<sonora::update::Fetcher> MakeHttpFetcher() {
  return nullptr;
}

}  // namespace sonora::platform
