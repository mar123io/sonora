// The tools an agent may be told about, and the rule that decides whether a proposed call is
// one of them.
//
// Generated from schema/bridge.schema.json, which is also where the bridge itself comes from,
// so a method cannot be exposed to the agent in one place and renamed in another. The
// generated half is GeneratedTools(); everything else in this file is the part that says no.
//
// Nothing here knows about CEF, about an operating system, or about a model. It is a list and
// a validator, and it has a test file pointed at it.

#ifndef SONORA_AGENT_TOOLS_H
#define SONORA_AGENT_TOOLS_H

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sonora::agent {

// What the broker is allowed to do with a call to this tool.
//
// Two classes and not three. `read` runs on sight, and that is safe here for a reason worth
// stating rather than assuming: no method in this bridge takes a destination -- no url, no
// path, no recipient -- so the worst a read can do with arguments somebody else chose is
// return the wrong rows. The day a read gains an argument that says *where*, `kRead` stops
// being a safe class and this comment is the thing that should stop somebody adding it.
enum class Effect {
  kRead,     // run it
  kConfirm,  // propose it, and run it only once a person has accepted the plan
};

[[nodiscard]] std::string_view Describe(Effect effect);

enum class ValueType { kString, kInt, kDouble, kBool };

struct Parameter {
  std::string name;
  ValueType type = ValueType::kString;
  bool array = false;
  bool required = true;
};

struct Tool {
  std::string name;        // "library.search" -- the bridge's own wire name
  std::string capability;  // the capability it belongs to, so negotiation can remove it
  Effect effect = Effect::kConfirm;
  std::string description;  // written for a planner, not for whoever maintains the bridge
  std::vector<Parameter> parameters;

  // The same parameters as JSON Schema, generated. Handed to a planner and never consulted
  // here: what the broker validates against is the typed list above, because a validator
  // that parses its own rules at runtime has two chances to disagree with itself.
  std::string_view parameters_json;
};

// Generated. Every method with an "agent" block in the schema, in schema order.
[[nodiscard]] std::vector<Tool> GeneratedTools();

// The tools actually offered, which is the generated list minus whatever this build has
// switched off.
//
// It takes the enabled capability names rather than reaching for the capability registry,
// so that a page running against a degraded build is offered a catalogue that matches what
// the bridge would accept -- and so that this stays testable without a shell.
class Catalogue {
 public:
  Catalogue() = default;
  Catalogue(std::vector<Tool> tools, const std::vector<std::string>& enabled_capabilities);

  // The generated catalogue, filtered the same way.
  [[nodiscard]] static Catalogue ForEnabled(
      const std::vector<std::string>& enabled_capabilities);

  [[nodiscard]] const std::vector<Tool>& tools() const { return tools_; }
  [[nodiscard]] std::size_t size() const { return tools_.size(); }
  [[nodiscard]] bool empty() const { return tools_.empty(); }

  // Nullptr when the name is not a tool this catalogue offers. A name the generated list
  // knows but this build has switched off is also nullptr, which is the answer that matters:
  // "the bridge has it" and "you may call it" are different questions.
  [[nodiscard]] const Tool* Find(std::string_view name) const;

  // Every tool, as the JSON array a planner is told about. Built from the generated schemas,
  // so this is a projection and not a second description.
  [[nodiscard]] std::string Describe() const;

 private:
  std::vector<Tool> tools_;
};

// Why a proposed call was refused. Each of these is a real case rather than a category: they
// are what the tests enumerate, and what the page shows the person.
enum class Refusal {
  kNone,
  kUnknownTool,       // not in the catalogue -- including withheld and switched off
  kUnknownArgument,   // a name the tool does not take
  kMissingArgument,   // a required one absent
  kWrongType,         // a string where a number belongs, or an array where a scalar does
  kNotFinite,         // a double that is NaN or infinity
  kTooManyArguments,  // beyond what any tool of this bridge takes
  kArrayTooLong,      // a list longer than the bridge would accept
};

[[nodiscard]] std::string_view Describe(Refusal refusal);

// Bounds. Not tuning: a plan is a thing somebody else composed, and every unbounded field in
// it is a way to spend this process's memory without asking.
inline constexpr std::size_t kMaxArguments = 16;
inline constexpr std::size_t kMaxArrayLength = 512;
inline constexpr std::size_t kMaxArgumentBytes = 4096;

// Checks a proposed call's arguments against the tool's parameter list.
//
// `arguments_json` is a JSON object as the planner produced it. Parsing happens here, because
// a call whose arguments do not parse is refused rather than repaired -- and because the one
// place that turns text into a decision should be the place with the tests.
[[nodiscard]] Refusal ValidateArguments(const Tool& tool,
                                        std::string_view arguments_json,
                                        std::string* offending_name = nullptr);

}  // namespace sonora::agent

#endif  // SONORA_AGENT_TOOLS_H
