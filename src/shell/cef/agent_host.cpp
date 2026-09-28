#include "cef/agent_host.h"

#include <utility>
#include <vector>

#include <bridge_generated.h>
#include <sonora/agent/planner.h>
#include <sonora/agent/tools.h>
#include <sonora/bridge/capabilities.h>
#include <sonora/bridge/protocol.h>
#include <nlohmann/json.hpp>

namespace sonora::shell {
namespace {

// The capability names this build still has, which is what decides the agent's catalogue.
//
// Read once, at construction. A capability can be switched off at runtime -- a machine with no
// sound card loses the transport -- but that happens before anything can dispatch, which is
// also before this object exists.
std::vector<std::string> EnabledNames(const bridge::CapabilityRegistry& capabilities) {
  std::vector<std::string> names;
  for (const bridge::CapabilityState& state : capabilities.all()) {
    if (state.enabled) {
      names.emplace_back(state.name);
    }
  }
  return names;
}

}  // namespace

AgentHost::AgentHost(bridge::BridgeHandlers& handlers,
                     const bridge::CapabilityRegistry& capabilities) {
  // Through Dispatch, and deliberately not straight into the handler. Building the same
  // envelope the page sends means the capability check, the error vocabulary and every limit
  // the handlers enforce are the ones already in use, rather than a second path that is
  // correct on the day it is written.
  //
  // It also means the metrics count a plan's reads as queries, which they are.
  agent::Invoker invoker = [&handlers, &capabilities](std::string_view tool,
                                                      std::string_view arguments_json) {
    nlohmann::json request;
    request["method"] = std::string(tool);
    request["params"] = arguments_json.empty()
                            ? nlohmann::json::object()
                            : nlohmann::json::parse(arguments_json, nullptr,
                                                    /*allow_exceptions=*/false);
    if (request["params"].is_discarded()) {
      // The broker validated these arguments before handing them over, so this cannot happen
      // from a plan -- which is exactly why it is worth answering rather than asserting.
      request["params"] = nlohmann::json::object();
    }

    const bridge::Response response = bridge::Dispatch(handlers, capabilities, request.dump());
    if (response.ok) {
      return response.payload;
    }

    // A failure is data too. It goes back to the planner as the result of that step, so a
    // second round sees what happened instead of an empty object that looks like success.
    nlohmann::json failed;
    failed["error"] = response.message;
    failed["code"] = response.failure_code();
    return failed.dump();
  };

  broker_ = std::make_unique<agent::Broker>(
      agent::Catalogue::ForEnabled(EnabledNames(capabilities)),
      std::make_unique<agent::LocalPlanner>(), std::move(invoker));
}

}  // namespace sonora::shell
