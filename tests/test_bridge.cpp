#include <catch2/catch_test_macros.hpp>

#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>

#include <bridge_generated.h>
#include <sonora/bridge/capabilities.h>
#include <sonora/bridge/protocol.h>

using namespace sonora::bridge;

namespace {

// A stand-in for the real shell. Every test drives the bridge through
// Dispatch, which is the only surface CEF ever touches -- so these tests cover
// the same path production does, including the error translation.
class TestHandlers final : public BridgeHandlers {
 public:
  ShellGetVersionResult ShellGetVersion(const ShellGetVersionParams&) override {
    ShellGetVersionResult result;
    result.version = "0.3.0";
    result.gitDescribe = "v0.2-cef";
    result.cefVersion = "152.0.8";
    result.chromiumVersion = "152.0.7977.134";
    return result;
  }

  ShellEchoResult ShellEcho(const ShellEchoParams& params) override {
    if (params.message.empty()) {
      throw BridgeError(ErrorCode::kInvalidParams, "message must not be empty");
    }
    const std::int64_t repeat = params.repeat.value_or(1);
    if (repeat < 1 || repeat > 64) {
      throw BridgeError(ErrorCode::kInvalidParams, "repeat must be between 1 and 64");
    }
    if (params.message == "boom") {
      // Stands in for a handler with a bug: it throws something that is not a
      // BridgeError. The bridge must survive it.
      throw std::runtime_error("something the handler did not expect");
    }

    std::string repeated;
    repeated.reserve(params.message.size() * static_cast<std::size_t>(repeat));
    for (std::int64_t i = 0; i < repeat; ++i) {
      repeated += params.message;
    }

    ShellEchoResult result;
    result.lengthBytes = static_cast<std::int64_t>(repeated.size());
    result.message = std::move(repeated);
    return result;
  }

  ShellGetCapabilitiesResult ShellGetCapabilities(const ShellGetCapabilitiesParams&) override {
    ShellGetCapabilitiesResult result;
    result.protocolVersion = kProtocolVersion;
    for (const CapabilityState& state : registry.all()) {
      Capability capability;
      capability.name = state.name;
      capability.version = state.version;
      capability.enabled = state.enabled;
      result.capabilities.push_back(std::move(capability));
    }
    return result;
  }

  DiagnosticsGetMetricsResult DiagnosticsGetMetrics(
      const DiagnosticsGetMetricsParams&) override {
    DiagnosticsGetMetricsResult result;
    result.queriesHandled = 7;
    return result;
  }

  CapabilityRegistry registry{kCapabilities};
};

nlohmann::json PayloadOf(const Response& response) {
  return nlohmann::json::parse(response.payload);
}

}  // namespace

TEST_CASE("a method with no parameters round-trips", "[bridge]") {
  TestHandlers handlers;
  const Response response =
      Dispatch(handlers, handlers.registry, R"({"method":"shell.getVersion"})");

  REQUIRE(response.ok);
  REQUIRE(PayloadOf(response)["version"] == "0.3.0");
  REQUIRE(PayloadOf(response)["chromiumVersion"] == "152.0.7977.134");
}

TEST_CASE("parameters reach the handler", "[bridge]") {
  TestHandlers handlers;
  const Response response =
      Dispatch(handlers, handlers.registry,
               R"({"method":"shell.echo","params":{"message":"ab","repeat":3}})");

  REQUIRE(response.ok);
  REQUIRE(PayloadOf(response)["message"] == "ababab");
  REQUIRE(PayloadOf(response)["lengthBytes"] == 6);
}

TEST_CASE("an absent optional parameter takes its default", "[bridge]") {
  TestHandlers handlers;
  const Response response = Dispatch(handlers, handlers.registry,
                                     R"({"method":"shell.echo","params":{"message":"x"}})");

  REQUIRE(response.ok);
  REQUIRE(PayloadOf(response)["message"] == "x");
}

TEST_CASE("an array of records survives the round trip", "[bridge]") {
  // Parallel arrays -- names[] and versions[] -- were what this returned
  // before the generator understood record types. They agree about the order
  // right up until the day they do not.
  TestHandlers handlers;
  const Response response =
      Dispatch(handlers, handlers.registry, R"({"method":"shell.getCapabilities"})");

  REQUIRE(response.ok);
  const auto payload = PayloadOf(response);
  REQUIRE(payload["protocolVersion"] == kProtocolVersion);
  REQUIRE(payload["capabilities"].size() == std::size(kCapabilities));
  REQUIRE(payload["capabilities"][0]["name"] == "shell");
  REQUIRE(payload["capabilities"][0]["enabled"] == true);
}

TEST_CASE("a record type parses back out of its own JSON", "[bridge]") {
  const Capability parsed = Capability::FromJson(
      nlohmann::json::parse(R"({"name":"media","version":3,"enabled":false})"));

  REQUIRE(parsed.name == "media");
  REQUIRE(parsed.version == 3);
  REQUIRE_FALSE(parsed.enabled);
  REQUIRE(parsed.ToJson()["version"] == 3);
}

TEST_CASE("a record with a field of the wrong type is rejected", "[bridge]") {
  REQUIRE_THROWS_AS(
      Capability::FromJson(nlohmann::json::parse(R"({"name":1,"version":3,"enabled":false})")),
      BridgeError);
  REQUIRE_THROWS_AS(Capability::FromJson(nlohmann::json::parse(R"({"name":"media"})")),
                    BridgeError);
}

TEST_CASE("a request that is not JSON is rejected", "[bridge]") {
  TestHandlers handlers;
  const Response response = Dispatch(handlers, handlers.registry, "{not json at all");

  REQUIRE_FALSE(response.ok);
  REQUIRE(response.code == ErrorCode::kMalformedRequest);
}

TEST_CASE("a request without a method is rejected", "[bridge]") {
  TestHandlers handlers;
  const Response response = Dispatch(handlers, handlers.registry, R"({"params":{}})");

  REQUIRE_FALSE(response.ok);
  REQUIRE(response.code == ErrorCode::kMalformedRequest);
}

TEST_CASE("an unknown method is named in the error", "[bridge]") {
  TestHandlers handlers;
  const Response response =
      Dispatch(handlers, handlers.registry, R"({"method":"player.play"})");

  REQUIRE_FALSE(response.ok);
  REQUIRE(response.code == ErrorCode::kUnknownMethod);
  REQUIRE(response.message.find("player.play") != std::string::npos);
}

TEST_CASE("a missing required parameter is rejected", "[bridge]") {
  TestHandlers handlers;
  const Response response =
      Dispatch(handlers, handlers.registry, R"({"method":"shell.echo","params":{}})");

  REQUIRE_FALSE(response.ok);
  REQUIRE(response.code == ErrorCode::kInvalidParams);
  REQUIRE(response.message.find("message") != std::string::npos);
}

TEST_CASE("a parameter of the wrong type is rejected", "[bridge]") {
  TestHandlers handlers;
  const Response response = Dispatch(handlers, handlers.registry,
                                     R"({"method":"shell.echo","params":{"message":42}})");

  REQUIRE_FALSE(response.ok);
  REQUIRE(response.code == ErrorCode::kInvalidParams);
}

TEST_CASE("a fractional number is not an integer", "[bridge]") {
  // JSON has a single number type; the schema does not have to inherit that.
  // Accepting 1.5 for an int field is how a rounding bug gets in for free.
  TestHandlers handlers;
  const Response response =
      Dispatch(handlers, handlers.registry,
               R"({"method":"shell.echo","params":{"message":"x","repeat":1.5}})");

  REQUIRE_FALSE(response.ok);
  REQUIRE(response.code == ErrorCode::kInvalidParams);
}

TEST_CASE("a handler's own validation reaches the caller", "[bridge]") {
  TestHandlers handlers;
  const Response response =
      Dispatch(handlers, handlers.registry,
               R"({"method":"shell.echo","params":{"message":"x","repeat":9999}})");

  REQUIRE_FALSE(response.ok);
  REQUIRE(response.code == ErrorCode::kInvalidParams);
  REQUIRE(response.message.find("64") != std::string::npos);
}

TEST_CASE("a handler that throws something unexpected does not escape", "[bridge][security]") {
  // Dispatch is called from a CEF callback, which cannot handle an exception.
  // Anything a handler throws has to stop here.
  TestHandlers handlers;
  const Response response = Dispatch(handlers, handlers.registry,
                                     R"({"method":"shell.echo","params":{"message":"boom"}})");

  REQUIRE_FALSE(response.ok);
  REQUIRE(response.code == ErrorCode::kInternalError);
}

TEST_CASE("a failed response names the method it came from", "[bridge]") {
  TestHandlers handlers;
  const Response response =
      Dispatch(handlers, handlers.registry, R"({"method":"shell.echo","params":{}})");

  REQUIRE_FALSE(response.ok);
  REQUIRE(response.method == "shell.echo");
}

TEST_CASE("error codes are stable numbers on the wire", "[bridge]") {
  // The page reads these as integers. Renumbering silently changes what an
  // older UI thinks happened.
  REQUIRE(static_cast<int>(ErrorCode::kMalformedRequest) == 1);
  REQUIRE(static_cast<int>(ErrorCode::kUnknownMethod) == 2);
  REQUIRE(static_cast<int>(ErrorCode::kInvalidParams) == 3);
  REQUIRE(static_cast<int>(ErrorCode::kUnavailable) == 4);
  REQUIRE(static_cast<int>(ErrorCode::kInternalError) == 5);
}
