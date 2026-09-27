#pragma once

#include <memory>

#include "sonora/update/updater.h"

namespace sonora::platform {

// One GET over https, which is the whole of Sonora's use of a network.
//
// The interface it returns is sonora::update::Fetcher, which is where the argument for
// the shape of this lives: everything a Fetcher hands back is checked against a
// signature or against a hash from a signed document before anything parses it, so this
// implementation is allowed to be the smallest thing that can fetch a file. No
// redirects beyond the ones the operating system's client follows, no caching, no
// resume, no retries, no parsing, and a hard cap on the body.
//
// Returns nothing on a platform that has no updater yet -- see
// shared/update_host_none.cpp for why that is an answer rather than an omission.
[[nodiscard]] std::unique_ptr<sonora::update::Fetcher> MakeHttpFetcher();

}  // namespace sonora::platform
