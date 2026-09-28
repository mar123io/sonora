// The thing that says no.
//
// A planner proposes calls; the broker decides whether any of them happen. Everything the
// planner returns is treated as data that arrived from outside this process -- because in the
// configuration this interface exists for, it did.
//
// Three rules, and they are the whole of ADR 0016:
//
//   1. A proposed call runs only if the catalogue has it. Not "if it is not forbidden": if it
//      is present. Methods without an "agent" block in the schema are not on the list the
//      broker consults, so no amount of being convincing reaches them.
//   2. A read runs on sight. Anything else is proposed to the person, and runs when they
//      accept it -- identified by a plan the broker issued, not by a name the page chose.
//   3. What a read returned goes back to the planner as data for one more round, and the
//      calls that come out of that round are validated exactly like the first. This is the
//      part that holds when a model is talked into something by a track title: the defence is
//      not that the model resists, it is that `player.clearQueue` is not in the catalogue.
//
// Portable. No CEF, no operating system, no network: the bridge is reached through one
// function the caller supplies, which is also what makes every one of those rules testable
// without a shell.

#ifndef SONORA_AGENT_BROKER_H
#define SONORA_AGENT_BROKER_H

#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "sonora/agent/planner.h"
#include "sonora/agent/tools.h"

namespace sonora::agent {

// Calls one method of the bridge and returns its result as JSON. Whatever the caller hands in
// is the only way this library can reach anything at all.
using Invoker =
    std::function<std::string(std::string_view tool, std::string_view arguments_json)>;

struct Step {
  std::string tool;
  std::string arguments_json;
  Effect effect = Effect::kConfirm;
  std::string result_json;  // filled in for a step that ran
};

struct Outcome {
  bool understood = false;
  std::string reply;

  // The reads that happened while working this out.
  std::vector<Step> performed;

  // What is waiting. Non-empty exactly when `plan_id` is, and nothing in it has run.
  std::string plan_id;
  std::vector<Step> pending;

  // Set when the broker refused a proposal outright. A refusal is not an error to hide: the
  // panel shows it, because "the planner asked for something it may not have" is the most
  // interesting thing this system can report about itself.
  Refusal refusal = Refusal::kNone;
  std::string refused_tool;
  std::string refused_argument;

  [[nodiscard]] bool refused() const { return refusal != Refusal::kNone; }
  [[nodiscard]] bool waiting() const { return !plan_id.empty(); }
};

class Broker {
 public:
  Broker(Catalogue catalogue, std::unique_ptr<Planner> planner, Invoker invoker);

  // One sentence in, one outcome out.
  [[nodiscard]] Outcome Interpret(std::string_view utterance);

  // Accepts or discards a plan the broker issued. An id it did not issue, or one already
  // resolved, is refused rather than ignored -- a confirmation that quietly does nothing is
  // indistinguishable from one that worked.
  [[nodiscard]] Outcome Resolve(std::string_view plan_id, bool accept);

  [[nodiscard]] const Catalogue& catalogue() const { return catalogue_; }
  [[nodiscard]] std::string planner_description() const { return planner_->description(); }
  [[nodiscard]] std::size_t waiting_plans() const { return pending_.size(); }

  // An utterance longer than this is refused before the planner sees it.
  static constexpr std::size_t kMaxUtteranceBytes = 2048;

  // How many rounds a single sentence may take: one to look, one to act. Not a tuning
  // parameter -- it is the difference between a loop with an end and a loop.
  static constexpr int kMaxRounds = 2;

  // How many plans may await a decision. Oldest dropped, because a plan nobody answered is a
  // plan somebody stopped caring about, and an unbounded list of them is memory the page can
  // spend by typing.
  static constexpr std::size_t kMaxPendingPlans = 4;

 private:
  struct Pending {
    std::string id;
    std::vector<Step> steps;
  };

  // Validates one proposed call. Fills the refusal fields of `outcome` and returns false when
  // the call may not happen.
  [[nodiscard]] bool Admit(const ToolCall& call, const Tool** tool, Outcome& outcome) const;

  [[nodiscard]] std::string IssueId();

  Catalogue catalogue_;
  std::unique_ptr<Planner> planner_;
  Invoker invoker_;
  std::deque<Pending> pending_;
  std::uint64_t issued_ = 0;
};

}  // namespace sonora::agent

#endif  // SONORA_AGENT_BROKER_H
