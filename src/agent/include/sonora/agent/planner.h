// What turns a sentence into proposed tool calls, and the one implementation that needs no
// network.
//
// The interface exists so that the thing in this project which is interesting -- the broker,
// the catalogue, the refusals -- can be built and tested without a model, and so that plugging
// a real provider in later changes one class rather than the design. A planner in this codebase
// has no authority: everything it returns is a *proposal*, and the broker validates every part
// of it against the catalogue before anything happens. That is not defence in depth, it is the
// only defence there is -- see ADR 0016.

#ifndef SONORA_AGENT_PLANNER_H
#define SONORA_AGENT_PLANNER_H

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "sonora/agent/tools.h"

namespace sonora::agent {

struct ToolCall {
  std::string tool;            // a wire name, as proposed -- not yet known to exist
  std::string arguments_json;  // a JSON object, as proposed -- not yet known to be valid
};

// Something a previous step returned, handed back so that a second round can use it.
//
// `result_json` is the bridge's own answer, and it contains whatever is in the person's
// library: track titles, album names, text somebody else chose. A planner may take values out
// of it to build arguments. A planner may not take instructions out of it, and the reason this
// interface cannot enforce that is the reason the broker re-validates everything afterwards.
struct Observation {
  std::string tool;
  std::string result_json;
};

struct Request {
  std::string utterance;
  const Catalogue* catalogue = nullptr;
  std::vector<Observation> observations;
};

struct Plan {
  // False when the planner does not know what was meant. Not an error: "I did not understand
  // that" is a better answer than a guessed action, and it is the answer a confirmation
  // dialogue cannot rescue somebody from.
  bool understood = false;
  std::string reply;  // one sentence, shown to the person next to the plan
  std::vector<ToolCall> calls;
};

class Planner {
 public:
  virtual ~Planner() = default;

  // Proposes. The name is the contract.
  [[nodiscard]] virtual Plan Propose(const Request& request) = 0;

  // For the log and for the panel: "local patterns", or a provider and model.
  [[nodiscard]] virtual std::string description() const = 0;
};

// A planner with no model in it.
//
// It matches a small grammar -- a verb, and optionally something to look for -- and it says so.
// It does not understand a mood: asked for "something quiet to work to" it searches for the
// word, tells the person that is what it did, and leaves the rest to whoever plugs a real
// provider into the same interface. A local planner that pretended otherwise would be the one
// dishonest thing in this repository.
class LocalPlanner final : public Planner {
 public:
  [[nodiscard]] Plan Propose(const Request& request) override;
  [[nodiscard]] std::string description() const override { return "local patterns"; }

  // How many track ids a single proposal will queue. Small on purpose: "play something" should
  // not become a hundred-track queue nobody asked for.
  static constexpr std::size_t kMaxQueued = 20;
};

}  // namespace sonora::agent

#endif  // SONORA_AGENT_PLANNER_H
