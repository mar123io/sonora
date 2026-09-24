#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <string>

#include <bridge_generated.h>
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

  ShellListCapabilitiesResult ShellListCapabilities(
      const ShellListCapabilitiesParams&) override {
    ShellListCapabilitiesResult result;
    for (const auto& capability : kCapabilities) {
      result.names.emplace_back(capability.name);
      result.versions.push_back(capability.version);
    }
    return result;
  }
};

nlohmann::json PayloadOf(const Response& response) {
  return nlohmann::json::parse(response.payload);
}

}  // namespace

TEST_CASE("a method with no parameters round-trips", "[bridge]") {
  TestHandlers handlers;
  const Response response = Dispatch(handlers, R"({"method":"shell.getVersion"})");

  REQUIRE(response.ok);
  REQUIRE(PayloadOf(response)["version"] == "0.3.0");
  REQUIRE(PayloadOf(response)["chromiumVersion"] == "152.0.7977.134");
}

TEST_CASE("parameters reach the handler", "[bridge]") {
  TestHandlers handlers;
  const Response response =
      Dispatch(handlers, R"({"method":"shell.echo","params":{"message":"ab","repeat":3}})");

  REQUIRE(response.ok);
  REQUIRE(PayloadOf(response)["message"] == "ababab");
  REQUIRE(PayloadOf(response)["lengthBytes"] == 6);
}

TEST_CASE("an absent optional parameter takes its default", "[bridge]") {
  TestHandlers handlers;
  const Response response =
      Dispatch(handlers, R"({"method":"shell.echo","params":{"message":"x"}})");

  REQUIRE(response.ok);
  REQUIRE(PayloadOf(response)["message"] == "x");
}

TEST_CASE("arrays survive the round trip", "[bridge]") {
  TestHandlers handlers;
  const Response response = Dispatch(handlers, R"({"method":"shell.listCapabilities"})");

  REQUIRE(response.ok);
  const auto payload = PayloadOf(response);
  REQUIRE(payload["names"].size() == 1);
  REQUIRE(payload["names"][0] == "shell");
  REQUIRE(payload["versions"][0] == 1);
}

TEST_CASE("a request that is not JSON is rejected", "[bridge]") {
  TestHandlers handlers;
  const Response response = Dispatch(handlers, "{not json at all");

  REQUIRE_FALSE(response.ok);
  REQUIRE(response.code == ErrorCode::kMalformedRequest);
}

TEST_CASE("a request without a method is rejected", "[bridge]") {
  TestHandlers handlers;
  const Response response = Dispatch(handlers, R"({"params":{}})");

  REQUIRE_FALSE(response.ok);
  REQUIRE(response.code == ErrorCode::kMalformedRequest);
}

TEST_CASE("an unknown method is named in the error", "[bridge]") {
  TestHandlers handlers;
  const Response response = Dispatch(handlers, R"({"method":"player.play"})");

  REQUIRE_FALSE(response.ok);
  REQUIRE(response.code == ErrorCode::kUnknownMethod);
  REQUIRE(response.message.find("player.play") != std::string::npos);
}

TEST_CASE("a missing required parameter is rejected", "[bridge]") {
  TestHandlers handlers;
  const Response response = Dispatch(handlers, R"({"method":"shell.echo","params":{}})");

  REQUIRE_FALSE(response.ok);
  REQUIRE(response.code == ErrorCode::kInvalidParams);
  REQUIRE(response.message.find("message") != std::string::npos);
}

TEST_CASE("a parameter of the wrong type is rejected", "[bridge]") {
  TestHandlers handlers;
  const Response response =
      Dispatch(handlers, R"({"method":"shell.echo","params":{"message":42}})");

  REQUIRE_FALSE(response.ok);
  REQUIRE(response.code == ErrorCode::kInvalidParams);
}

TEST_CASE("a fractional number is not an integer", "[bridge]") {
  // JSON has a single number type; the schema does not have to inherit that.
  // Accepting 1.5 for an int field is how a rounding bug gets in for free.
  TestHandlers handlers;
  const Response response =
      Dispatch(handlers, R"({"method":"shell.echo","params":{"message":"x","repeat":1.5}})");

  REQUIRE_FALSE(response.ok);
  REQUIRE(response.code == ErrorCode::kInvalidParams);
}

TEST_CASE("a handler's own validation reaches the caller", "[bridge]") {
  TestHandlers handlers;
  const Response response =
      Dispatch(handlers, R"({"method":"shell.echo","params":{"message":"x","repeat":9999}})");

  REQUIRE_FALSE(response.ok);
  REQUIRE(response.code == ErrorCode::kInvalidParams);
  REQUIRE(response.message.find("64") != std::string::npos);
}

TEST_CASE("a handler that throws something unexpected does not escape", "[bridge][security]") {
  // Dispatch is called from a CEF callback, which cannot handle an exception.
  // Anything a handler throws has to stop here.
  TestHandlers handlers;
  const Response response =
      Dispatch(handlers, R"({"method":"shell.echo","params":{"message":"boom"}})");

  REQUIRE_FALSE(response.ok);
  REQUIRE(response.code == ErrorCode::kInternalError);
}

TEST_CASE("a failed response names the method it came from", "[bridge]") {
  TestHandlers handlers;
  const Response response = Dispatch(handlers, R"({"method":"shell.echo","params":{}})");

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
