#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <bridge_generated.h>
#include <sonora/bridge/capabilities.h>

using namespace sonora::bridge;

namespace {

// A table of its own rather than the generated one, so these tests keep
// meaning the same thing when a capability is added to the schema.
constexpr CapabilityInfo kTable[] = {
    {"shell", 2, true},
    {"diagnostics", 1, false},
    {"media", 3, false},
};

[[nodiscard]] bool Contains(const std::vector<std::string>& haystack,
                            const std::string& needle) {
  for (const std::string& item : haystack) {
    if (item == needle) {
      return true;
    }
  }
  return false;
}

}  // namespace

TEST_CASE("an empty disable list leaves everything on", "[capabilities]") {
  const CapabilityRegistry registry(kTable, "");

  REQUIRE(registry.all().size() == 3);
  REQUIRE(registry.IsEnabled("shell"));
  REQUIRE(registry.IsEnabled("diagnostics"));
  REQUIRE(registry.unknown().empty());
  REQUIRE(registry.refused().empty());
}

TEST_CASE("a named capability is switched off", "[capabilities]") {
  const CapabilityRegistry registry(kTable, "diagnostics");

  REQUIRE_FALSE(registry.IsEnabled("diagnostics"));
  REQUIRE(registry.IsEnabled("shell"));
  REQUIRE(registry.IsEnabled("media"));
}

TEST_CASE("the list is split on commas and trimmed", "[capabilities]") {
  const CapabilityRegistry registry(kTable, "  diagnostics , media ,, ");

  REQUIRE_FALSE(registry.IsEnabled("diagnostics"));
  REQUIRE_FALSE(registry.IsEnabled("media"));
  REQUIRE(registry.unknown().empty());
}

TEST_CASE("splitting handles the shapes an environment variable actually takes",
          "[capabilities]") {
  REQUIRE(detail::SplitList("").empty());
  REQUIRE(detail::SplitList("   ").empty());
  REQUIRE(detail::SplitList(",,,").empty());
  REQUIRE(detail::SplitList("one").size() == 1);
  REQUIRE(detail::SplitList("one,two").size() == 2);
  REQUIRE(detail::SplitList("one,").size() == 1);
  REQUIRE(detail::SplitList(",one").size() == 1);
  REQUIRE(detail::SplitList("\tone \r\n")[0] == "one");
}

TEST_CASE("a required capability cannot be switched off", "[capabilities]") {
  // Disabling the capability that reports the capabilities leaves the page no
  // way to discover what happened. The registry keeps it and says so.
  const CapabilityRegistry registry(kTable, "shell");

  REQUIRE(registry.IsEnabled("shell"));
  REQUIRE(Contains(registry.refused(), "shell"));
  REQUIRE(registry.Summary().find("required") != std::string::npos);
}

TEST_CASE("a name that is not a capability is reported, not ignored", "[capabilities]") {
  // A typo in an environment variable is silent by nature: the feature simply
  // stays on and nobody knows why.
  const CapabilityRegistry registry(kTable, "diagnostic");

  REQUIRE(Contains(registry.unknown(), "diagnostic"));
  REQUIRE(registry.IsEnabled("diagnostics"));
  REQUIRE(registry.Summary().find("diagnostic") != std::string::npos);
}

TEST_CASE("asking after an unknown capability is not the same as a disabled one",
          "[capabilities]") {
  const CapabilityRegistry registry(kTable, "");

  REQUIRE_FALSE(registry.IsEnabled("nothingLikeThis"));
  REQUIRE_THROWS_AS(registry.Require("nothingLikeThis"), BridgeError);
}

TEST_CASE("Require reports a disabled capability as unavailable", "[capabilities]") {
  const CapabilityRegistry registry(kTable, "diagnostics");

  REQUIRE_NOTHROW(registry.Require("shell"));
  try {
    registry.Require("diagnostics");
    FAIL("Require should have thrown");
  } catch (const BridgeError& error) {
    // Not kUnknownMethod: the page has to be able to tell "too old for you"
    // from "that has never existed".
    REQUIRE(error.code() == ErrorCode::kUnavailable);
    REQUIRE(std::string(error.what()).find("diagnostics") != std::string::npos);
  }
}

TEST_CASE("the summary says what is on and what is off", "[capabilities]") {
  const CapabilityRegistry registry(kTable, "media");
  const std::string summary = registry.Summary();

  REQUIRE(summary.find("shell@2") != std::string::npos);
  REQUIRE(summary.find("disabled: media@3") != std::string::npos);
}

TEST_CASE("the generated table is what the registry is built from", "[capabilities]") {
  // The schema is the only place a capability is declared. If this ever needs
  // changing by hand, something has stopped being generated.
  const CapabilityRegistry registry(kCapabilities, "");

  REQUIRE(registry.IsEnabled("shell"));
  REQUIRE(registry.IsEnabled("diagnostics"));
  for (const CapabilityState& state : registry.all()) {
    REQUIRE(state.version >= 1);
  }
}

TEST_CASE("a disabled capability's methods are unavailable, not unknown",
          "[capabilities][bridge]") {
  // The whole point of the registry: dispatch answers differently, and the
  // difference is the one the UI branches on.
  struct Handlers final : BridgeHandlers {
    int diagnostics_calls = 0;

    ShellGetVersionResult ShellGetVersion(const ShellGetVersionParams&) override { return {}; }
    ShellEchoResult ShellEcho(const ShellEchoParams&) override { return {}; }
    ShellGetCapabilitiesResult ShellGetCapabilities(
        const ShellGetCapabilitiesParams&) override {
      return {};
    }
    // Counted rather than FAIL()ed: a handler that throws its way out of the
    // test leaves everything after it unrun, and the assertion belongs in the
    // test body next to the response it is about.
    DiagnosticsGetMetricsResult DiagnosticsGetMetrics(
        const DiagnosticsGetMetricsParams&) override {
      ++diagnostics_calls;
      return {};
    }
  };

  Handlers handlers;
  const CapabilityRegistry registry(kCapabilities, "diagnostics");

  const Response disabled =
      Dispatch(handlers, registry, R"({"method":"diagnostics.getMetrics"})");
  REQUIRE_FALSE(disabled.ok);
  REQUIRE(disabled.code == ErrorCode::kUnavailable);
  // Refused before the handler, not by it: a capability that is off must not
  // run any of the work behind it.
  REQUIRE(handlers.diagnostics_calls == 0);

  const Response missing =
      Dispatch(handlers, registry, R"({"method":"diagnostics.notAThing"})");
  REQUIRE_FALSE(missing.ok);
  REQUIRE(missing.code == ErrorCode::kUnknownMethod);

  // And the required capability still answers, which is what makes recovery
  // possible at all.
  const Response negotiate =
      Dispatch(handlers, registry, R"({"method":"shell.getCapabilities"})");
  REQUIRE(negotiate.ok);
}
