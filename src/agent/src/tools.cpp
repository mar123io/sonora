#include "sonora/agent/tools.h"

#include <algorithm>
#include <cmath>

#include <nlohmann/json.hpp>

namespace sonora::agent {

std::string_view Describe(Effect effect) {
  switch (effect) {
    case Effect::kRead:
      return "read";
    case Effect::kConfirm:
      return "confirm";
  }
  return "unknown";
}

std::string_view Describe(Refusal refusal) {
  switch (refusal) {
    case Refusal::kNone:
      return "allowed";
    case Refusal::kUnknownTool:
      return "not a tool this build offers";
    case Refusal::kUnknownArgument:
      return "an argument the tool does not take";
    case Refusal::kMissingArgument:
      return "a required argument is missing";
    case Refusal::kWrongType:
      return "an argument is the wrong type";
    case Refusal::kNotFinite:
      return "a number that is not finite";
    case Refusal::kTooManyArguments:
      return "more arguments than any tool takes";
    case Refusal::kArrayTooLong:
      return "a list longer than the bridge accepts";
  }
  return "refused";
}

Catalogue::Catalogue(std::vector<Tool> tools,
                     const std::vector<std::string>& enabled_capabilities) {
  tools_.reserve(tools.size());
  for (Tool& tool : tools) {
    const bool enabled = std::find(enabled_capabilities.begin(), enabled_capabilities.end(),
                                   tool.capability) != enabled_capabilities.end();
    if (enabled) {
      tools_.push_back(std::move(tool));
    }
  }
}

Catalogue Catalogue::ForEnabled(const std::vector<std::string>& enabled_capabilities) {
  return Catalogue(GeneratedTools(), enabled_capabilities);
}

const Tool* Catalogue::Find(std::string_view name) const {
  const auto found = std::find_if(tools_.begin(), tools_.end(),
                                  [name](const Tool& tool) { return tool.name == name; });
  return found == tools_.end() ? nullptr : &*found;
}

std::string Catalogue::Describe() const {
  nlohmann::json array = nlohmann::json::array();
  for (const Tool& tool : tools_) {
    nlohmann::json entry;
    entry["name"] = tool.name;
    entry["effect"] = agent::Describe(tool.effect);
    entry["description"] = tool.description;
    // Already JSON, and generated: parsed here rather than pasted, so that a malformed
    // generator output fails loudly in a test instead of producing a document that looks
    // like JSON and is not.
    entry["parameters"] = nlohmann::json::parse(tool.parameters_json);
    array.push_back(std::move(entry));
  }
  return array.dump();
}

namespace {

// Whether one JSON value is the type a parameter declares. Deliberately strict: a string
// that looks like a number is not a number, because the alternative is a validator that
// accepts "12; DROP" for an int on some future day when somebody adds a coercion.
bool Matches(const nlohmann::json& value, ValueType type) {
  switch (type) {
    case ValueType::kString:
      return value.is_string();
    case ValueType::kInt:
      // is_number_integer() is true for a bool in some versions' intent but not here: JSON
      // booleans are their own type in nlohmann, and an int parameter given `true` is a
      // planner that has guessed.
      return value.is_number_integer() && !value.is_boolean();
    case ValueType::kDouble:
      return value.is_number();
    case ValueType::kBool:
      return value.is_boolean();
  }
  return false;
}

bool Finite(const nlohmann::json& value, ValueType type) {
  if (type != ValueType::kDouble || !value.is_number_float()) {
    return true;
  }
  const double number = value.get<double>();
  return std::isfinite(number);
}

}  // namespace

Refusal ValidateArguments(const Tool& tool,
                          std::string_view arguments_json,
                          std::string* offending_name) {
  const auto blame = [offending_name](std::string name, Refusal refusal) {
    if (offending_name != nullptr) {
      *offending_name = std::move(name);
    }
    return refusal;
  };

  if (arguments_json.size() > kMaxArgumentBytes) {
    return blame({}, Refusal::kTooManyArguments);
  }

  // An absent or empty argument object is an empty object, because "no arguments" and "{}"
  // are the same statement and a tool with no required parameters should accept both.
  nlohmann::json arguments = nlohmann::json::object();
  if (!arguments_json.empty()) {
    arguments = nlohmann::json::parse(arguments_json, nullptr, /*allow_exceptions=*/false);
    if (arguments.is_discarded() || !arguments.is_object()) {
      return blame({}, Refusal::kWrongType);
    }
  }
  if (arguments.size() > kMaxArguments) {
    return blame({}, Refusal::kTooManyArguments);
  }

  // Every argument given has to be one the tool takes. This is the direction that matters:
  // checking only that the required ones are present would let anything else through, and
  // "anything else" is where a field nobody validates goes.
  for (const auto& [name, value] : arguments.items()) {
    const auto parameter =
        std::find_if(tool.parameters.begin(), tool.parameters.end(),
                     [&name](const Parameter& candidate) { return candidate.name == name; });
    if (parameter == tool.parameters.end()) {
      return blame(name, Refusal::kUnknownArgument);
    }

    if (parameter->array) {
      if (!value.is_array()) {
        return blame(name, Refusal::kWrongType);
      }
      if (value.size() > kMaxArrayLength) {
        return blame(name, Refusal::kArrayTooLong);
      }
      for (const auto& element : value) {
        if (!Matches(element, parameter->type)) {
          return blame(name, Refusal::kWrongType);
        }
        if (!Finite(element, parameter->type)) {
          return blame(name, Refusal::kNotFinite);
        }
      }
      continue;
    }

    if (!Matches(value, parameter->type)) {
      return blame(name, Refusal::kWrongType);
    }
    if (!Finite(value, parameter->type)) {
      return blame(name, Refusal::kNotFinite);
    }
  }

  for (const Parameter& parameter : tool.parameters) {
    if (parameter.required && !arguments.contains(parameter.name)) {
      return blame(parameter.name, Refusal::kMissingArgument);
    }
  }

  return blame({}, Refusal::kNone);
}

}  // namespace sonora::agent
