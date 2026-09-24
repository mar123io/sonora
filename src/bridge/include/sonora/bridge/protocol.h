#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <nlohmann/json.hpp>

// The hand-written half of the bridge: the envelope, the error vocabulary and
// the field readers the generated code calls. Everything method-specific is
// generated from schema/bridge.schema.json.
//
// Deliberately free of CEF. The transport moves two strings -- a request in and
// a response out -- so this layer is testable on any platform, which is what
// lets the macOS and Linux CI jobs exercise it without a gigabyte of Chromium.

namespace sonora::bridge {

// Sent to the page as the numeric code of a failed query. Values are part of
// the wire protocol: append, never renumber.
enum class ErrorCode : int {
  kNone = 0,
  kMalformedRequest = 1,  // not JSON, or no method name
  kUnknownMethod = 2,     // the shell has never heard of it
  kInvalidParams = 3,     // right method, wrong arguments
  kUnavailable = 4,       // the capability exists but is off in this build
  kInternalError = 5,     // a handler failed in a way it did not anticipate
};

[[nodiscard]] std::string_view ToString(ErrorCode code) noexcept;

// Thrown by handlers and by the generated parsers. Dispatch is the only thing
// that catches it; it never escapes into CEF.
class BridgeError : public std::runtime_error {
 public:
  BridgeError(ErrorCode code, const std::string& message)
      : std::runtime_error(message), code_(code) {}

  [[nodiscard]] ErrorCode code() const noexcept { return code_; }

 private:
  ErrorCode code_;
};

// A capability as the schema declares it. The table itself is generated; this
// is the shape, here rather than in the generated header so that
// CapabilityRegistry can be written by hand against it without the two files
// including each other.
struct CapabilityInfo {
  std::string_view name;
  int version = 0;
  // A required capability cannot be switched off. Something has to answer
  // "what do you support?", and it cannot be the thing being negotiated.
  bool required = false;
};

// { "method": "shell.echo", "params": { ... } }
struct Request {
  std::string method;
  nlohmann::json params = nlohmann::json::object();

  // Throws BridgeError(kMalformedRequest) on anything it cannot make sense of.
  [[nodiscard]] static Request Parse(std::string_view request_json);
};

struct Response {
  bool ok = false;
  std::string payload;  // the result, as JSON, when ok
  ErrorCode code = ErrorCode::kNone;
  std::string message;  // human-readable, for the console and the log
  std::string method;   // echoed back so a log line says what failed

  [[nodiscard]] static Response Ok(const nlohmann::json& result);
  [[nodiscard]] static Response Failed(ErrorCode code,
                                       const std::string& message,
                                       const std::string& method = {});

  // CEF's failure callback takes an int and a string.
  [[nodiscard]] int failure_code() const noexcept { return static_cast<int>(code); }
};

namespace detail {
[[nodiscard]] std::string TypeMismatch(std::string_view field, std::string_view expected);
}  // namespace detail

// Readers used by the generated parsers. They exist so the generated code stays
// short enough to read: one line per field, and the error messages are written
// once here rather than repeated by a code generator.
template <typename T>
[[nodiscard]] T ReadScalar(const nlohmann::json& value, std::string_view field) {
  if constexpr (std::is_same_v<T, std::string>) {
    if (!value.is_string()) {
      throw BridgeError(ErrorCode::kInvalidParams, detail::TypeMismatch(field, "a string"));
    }
    return value.get<std::string>();
  } else if constexpr (std::is_same_v<T, std::int64_t>) {
    // is_number_integer() is false for 1.5 and true for 1, which is the
    // distinction the schema means by "int". JSON has one number type; the
    // schema does not have to inherit that.
    if (!value.is_number_integer()) {
      throw BridgeError(ErrorCode::kInvalidParams, detail::TypeMismatch(field, "an integer"));
    }
    return value.get<std::int64_t>();
  } else if constexpr (std::is_same_v<T, double>) {
    if (!value.is_number()) {
      throw BridgeError(ErrorCode::kInvalidParams, detail::TypeMismatch(field, "a number"));
    }
    return value.get<double>();
  } else if constexpr (std::is_same_v<T, bool>) {
    if (!value.is_boolean()) {
      throw BridgeError(ErrorCode::kInvalidParams, detail::TypeMismatch(field, "a boolean"));
    }
    return value.get<bool>();
  } else {
    static_assert(sizeof(T) == 0, "unsupported scalar type in the bridge schema");
  }
}

template <typename T>
[[nodiscard]] std::vector<T> ReadArray(const nlohmann::json& value, std::string_view field) {
  if (!value.is_array()) {
    throw BridgeError(ErrorCode::kInvalidParams, detail::TypeMismatch(field, "an array"));
  }
  std::vector<T> out;
  out.reserve(value.size());
  for (const auto& item : value) {
    out.push_back(ReadScalar<T>(item, field));
  }
  return out;
}

// The same two, for the record types declared under "types" in the schema. T
// supplies its own FromJson, which the generator wrote from the same
// description, so the field-level errors inside it read the same as these.
template <typename T>
[[nodiscard]] T ReadStruct(const nlohmann::json& value, std::string_view field) {
  if (!value.is_object()) {
    throw BridgeError(ErrorCode::kInvalidParams, detail::TypeMismatch(field, "an object"));
  }
  return T::FromJson(value);
}

template <typename T>
[[nodiscard]] std::vector<T> ReadStructArray(const nlohmann::json& value,
                                             std::string_view field) {
  if (!value.is_array()) {
    throw BridgeError(ErrorCode::kInvalidParams, detail::TypeMismatch(field, "an array"));
  }
  std::vector<T> out;
  out.reserve(value.size());
  for (const auto& item : value) {
    out.push_back(ReadStruct<T>(item, field));
  }
  return out;
}

}  // namespace sonora::bridge
