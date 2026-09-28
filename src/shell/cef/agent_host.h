#pragma once

#include <memory>
#include <string>
#include <string_view>

#include <sonora/agent/broker.h>

namespace sonora::bridge {
class BridgeHandlers;
class CapabilityRegistry;
}  // namespace sonora::bridge

namespace sonora::shell {

// Holds the broker, and gives it the one function it needs to reach the rest of the
// application.
//
// That function goes through bridge::Dispatch -- the same entry point the page uses, with the
// same capability check and the same handler. The agent gets no side door: a tool call from a
// plan is indistinguishable, once it is past the broker, from the page having asked for it, so
// every limit the handlers already enforce (five thousand rows, sixty-four echoes, an id that
// is either in the index or is not) applies unchanged and without being restated here.
//
// Constructed after the handlers it dispatches into, which is why it takes them by reference
// and why the runtime builds it last.
class AgentHost {
 public:
  AgentHost(bridge::BridgeHandlers& handlers, const bridge::CapabilityRegistry& capabilities);

  [[nodiscard]] agent::Broker& broker() { return *broker_; }
  [[nodiscard]] const agent::Broker& broker() const { return *broker_; }

 private:
  std::unique_ptr<agent::Broker> broker_;
};

}  // namespace sonora::shell
