#include "sonora/agent/broker.h"

#include <algorithm>
#include <utility>

namespace sonora::agent {

Broker::Broker(Catalogue catalogue, std::unique_ptr<Planner> planner, Invoker invoker)
    : catalogue_(std::move(catalogue)),
      planner_(std::move(planner)),
      invoker_(std::move(invoker)) {}

bool Broker::Admit(const ToolCall& call, const Tool** tool, Outcome& outcome) const {
  // Present, not un-forbidden. The catalogue is the list; everything else the bridge can do is
  // absent from it rather than denied by it, and that difference is the design.
  const Tool* found = catalogue_.Find(call.tool);
  if (found == nullptr) {
    outcome.refusal = Refusal::kUnknownTool;
    outcome.refused_tool = call.tool;
    return false;
  }

  std::string offending;
  const Refusal refusal = ValidateArguments(*found, call.arguments_json, &offending);
  if (refusal != Refusal::kNone) {
    outcome.refusal = refusal;
    outcome.refused_tool = call.tool;
    outcome.refused_argument = std::move(offending);
    return false;
  }

  *tool = found;
  return true;
}

std::string Broker::IssueId() {
  // Issued here and never taken from the page. It binds an acceptance to the plan the person
  // was shown, and that is all it is: it is not a capability token and it cannot be one,
  // because the thing that displays the plan is also the thing that sends the acceptance. What
  // that assumption is worth, and what stops holding if the page is not Sonora's own, is
  // written down in ADR 0016 rather than implied by the length of this string.
  ++issued_;
  return "plan-" + std::to_string(issued_);
}

Outcome Broker::Interpret(std::string_view utterance) {
  Outcome outcome;

  if (utterance.size() > kMaxUtteranceBytes) {
    outcome.reply = "That is longer than I will read.";
    return outcome;
  }

  Request request;
  request.utterance = std::string(utterance);
  request.catalogue = &catalogue_;

  for (int round = 0; round < kMaxRounds; ++round) {
    const Plan plan = planner_->Propose(request);
    if (!plan.understood || plan.calls.empty()) {
      // A round can add to an outcome and never retract one. Without that rule a planner that
      // reads something and then has nothing further to say turns a successful answer into
      // "I did not understand" -- which is what the first version of this did, and what its
      // test caught.
      if (outcome.performed.empty()) {
        outcome.understood = plan.understood;
        outcome.reply = plan.reply;
      }
      return outcome;
    }
    outcome.understood = true;
    outcome.reply = plan.reply;

    // Admit the whole plan before running any of it. A plan is accepted or refused as a unit,
    // because half of a plan is a state nobody asked for -- and because a planner that gets
    // one legal call past the gate and one refused after it has still had an effect.
    std::vector<std::pair<const Tool*, const ToolCall*>> admitted;
    admitted.reserve(plan.calls.size());
    for (const ToolCall& call : plan.calls) {
      const Tool* tool = nullptr;
      if (!Admit(call, &tool, outcome)) {
        outcome.performed.clear();
        outcome.pending.clear();
        outcome.plan_id.clear();
        return outcome;
      }
      admitted.emplace_back(tool, &call);
    }

    const bool all_reads = std::all_of(admitted.begin(), admitted.end(), [](const auto& entry) {
      return entry.first->effect == Effect::kRead;
    });

    if (!all_reads) {
      // Anything that is not a read waits. The plan is handed back whole, including the reads
      // in it, so that what the person accepts is what they were shown.
      Pending held;
      held.id = IssueId();
      for (const auto& [tool, call] : admitted) {
        Step step;
        step.tool = call->tool;
        step.arguments_json = call->arguments_json;
        step.effect = tool->effect;
        held.steps.push_back(std::move(step));
      }
      outcome.plan_id = held.id;
      outcome.pending = held.steps;
      if (pending_.size() >= kMaxPendingPlans) {
        pending_.pop_front();
      }
      pending_.push_back(std::move(held));
      return outcome;
    }

    // Every call is a read, so run them and let the planner look at what came back.
    request.observations.clear();
    for (const auto& [tool, call] : admitted) {
      Step step;
      step.tool = call->tool;
      step.arguments_json = call->arguments_json;
      step.effect = tool->effect;
      step.result_json = invoker_ ? invoker_(call->tool, call->arguments_json) : std::string{};

      Observation observation;
      observation.tool = step.tool;
      observation.result_json = step.result_json;
      request.observations.push_back(std::move(observation));

      outcome.performed.push_back(std::move(step));
    }
  }

  // Two rounds of reads and nothing to propose. Not an error: the planner looked twice and had
  // nothing to act on, and the reads it did are in `performed` for the person to see.
  return outcome;
}

Outcome Broker::Resolve(std::string_view plan_id, bool accept) {
  Outcome outcome;

  const auto found =
      std::find_if(pending_.begin(), pending_.end(),
                   [plan_id](const Pending& plan) { return plan.id == plan_id; });
  if (found == pending_.end()) {
    // An id this broker did not issue, or one that has already been answered. Said out loud:
    // a confirmation that silently does nothing looks exactly like one that worked.
    outcome.refusal = Refusal::kUnknownTool;
    outcome.refused_tool = std::string(plan_id);
    outcome.reply = "That plan is not waiting for an answer.";
    return outcome;
  }

  Pending plan = std::move(*found);
  pending_.erase(found);  // single use, whichever way it was answered

  outcome.understood = true;
  if (!accept) {
    outcome.reply = "Nothing was done.";
    return outcome;
  }

  // Validated when it was proposed, and validated again now. The catalogue can have changed
  // underneath it -- a capability switched off between the proposal and the acceptance -- and
  // the cost of asking twice is one lookup per step.
  for (const Step& step : plan.steps) {
    ToolCall call;
    call.tool = step.tool;
    call.arguments_json = step.arguments_json;
    const Tool* tool = nullptr;
    if (!Admit(call, &tool, outcome)) {
      outcome.reply = "Refused on the way out: " + std::string(Describe(outcome.refusal));
      return outcome;
    }
  }

  for (Step step : plan.steps) {
    step.result_json = invoker_ ? invoker_(step.tool, step.arguments_json) : std::string{};
    outcome.performed.push_back(std::move(step));
  }
  outcome.reply = "Done.";
  return outcome;
}

}  // namespace sonora::agent
