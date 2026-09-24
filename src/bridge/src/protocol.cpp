#include <sonora/bridge/protocol.h>

namespace sonora::bridge {

std::string_view ToString(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::kNone:
      return "none";
    case ErrorCode::kMalformedRequest:
      return "malformed-request";
    case ErrorCode::kUnknownMethod:
      return "unknown-method";
    case ErrorCode::kInvalidParams:
      return "invalid-params";
    case ErrorCode::kUnavailable:
      return "unavailable";
    case ErrorCode::kInternalError:
      return "internal-error";
  }
  return "unknown";
}

namespace detail {

std::string TypeMismatch(std::string_view field, std::string_view expected) {
  return "field '" + std::string(field) + "' must be " + std::string(expected);
}

}  // namespace detail

Request Request::Parse(std::string_view request_json) {
  // The page is trusted code we shipped, but it is still the untrusted side of
  // this boundary: a renderer is one compromised dependency away from sending
  // anything at all. Every field is checked.
  const nlohmann::json value =
      nlohmann::json::parse(request_json, /*cb=*/nullptr, /*allow_exceptions=*/false);

  if (value.is_discarded()) {
    throw BridgeError(ErrorCode::kMalformedRequest, "request is not valid JSON");
  }
  if (!value.is_object()) {
    throw BridgeError(ErrorCode::kMalformedRequest, "request must be a JSON object");
  }
  if (!value.contains("method") || !value.at("method").is_string()) {
    throw BridgeError(ErrorCode::kMalformedRequest, "request has no 'method' string");
  }

  Request request;
  request.method = value.at("method").get<std::string>();
  if (request.method.empty()) {
    throw BridgeError(ErrorCode::kMalformedRequest, "'method' is empty");
  }

  if (value.contains("params") && !value.at("params").is_null()) {
    if (!value.at("params").is_object()) {
      throw BridgeError(ErrorCode::kMalformedRequest, "'params' must be an object");
    }
    request.params = value.at("params");
  }
  return request;
}

Response Response::Ok(const nlohmann::json& result) {
  Response response;
  response.ok = true;
  response.payload = result.dump();
  return response;
}

Response Response::Failed(ErrorCode code,
                          const std::string& message,
                          const std::string& method) {
  Response response;
  response.ok = false;
  response.code = code;
  response.message = message;
  response.method = method;
  return response;
}

}  // namespace sonora::bridge
