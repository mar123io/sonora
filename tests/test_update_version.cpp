#include <catch2/catch_test_macros.hpp>

#include <string>

#include "sonora/update/version.h"

using sonora::update::ParseVersion;
using sonora::update::ToString;
using sonora::update::Version;

TEST_CASE("a version is three numbers and nothing else") {
  const auto version = ParseVersion("1.2.3");
  REQUIRE(version.has_value());
  CHECK(version->major == 1);
  CHECK(version->minor == 2);
  CHECK(version->patch == 3);
}

TEST_CASE("zeros are versions") {
  const auto version = ParseVersion("0.0.0");
  REQUIRE(version.has_value());
  CHECK(version->is_zero());
  CHECK(ToString(*version) == "0.0.0");
}

TEST_CASE("what a version is not") {
  // The narrative tags this project uses as milestones. cmake/Version.cmake
  // excludes them with a --match pattern; this is the same rule, enforced where
  // the string arrives from a file instead of from git.
  CHECK_FALSE(ParseVersion("v1.2.3").has_value());
  CHECK_FALSE(ParseVersion("0.4-native").has_value());
  CHECK_FALSE(ParseVersion("0.5.0-2-g1fd9823").has_value());

  CHECK_FALSE(ParseVersion("").has_value());
  CHECK_FALSE(ParseVersion("1").has_value());
  CHECK_FALSE(ParseVersion("1.2").has_value());
  CHECK_FALSE(ParseVersion("1.2.3.4").has_value());
  CHECK_FALSE(ParseVersion("1..3").has_value());
  CHECK_FALSE(ParseVersion(".1.2").has_value());
  CHECK_FALSE(ParseVersion("1.2.").has_value());
  CHECK_FALSE(ParseVersion(" 1.2.3").has_value());
  CHECK_FALSE(ParseVersion("1.2.3 ").has_value());
  CHECK_FALSE(ParseVersion("1.2.3\n").has_value());
  CHECK_FALSE(ParseVersion("+1.2.3").has_value());
  CHECK_FALSE(ParseVersion("-1.2.3").has_value());
  CHECK_FALSE(ParseVersion("1.2.3a").has_value());
  CHECK_FALSE(ParseVersion("0x1.2.3").has_value());
}

TEST_CASE("a leading zero is refused so that a version has one spelling") {
  CHECK_FALSE(ParseVersion("1.02.3").has_value());
  CHECK_FALSE(ParseVersion("01.2.3").has_value());
  CHECK_FALSE(ParseVersion("1.2.00").has_value());
  // ...but a single zero is not a leading zero.
  CHECK(ParseVersion("1.0.3").has_value());
}

TEST_CASE("a component that does not fit in 32 bits is refused, not truncated") {
  CHECK(ParseVersion("4294967295.0.0").has_value());
  CHECK_FALSE(ParseVersion("4294967296.0.0").has_value());
  CHECK_FALSE(ParseVersion("99999999999999999999.0.0").has_value());
}

TEST_CASE("a very long string is refused before it is parsed") {
  CHECK_FALSE(ParseVersion(std::string(4096, '1')).has_value());
}

TEST_CASE("versions order by major, then minor, then patch") {
  CHECK(ParseVersion("0.9.9") < ParseVersion("1.0.0"));
  CHECK(ParseVersion("1.0.9") < ParseVersion("1.1.0"));
  CHECK(ParseVersion("1.1.1") < ParseVersion("1.1.2"));
  CHECK(ParseVersion("1.10.0") > ParseVersion("1.9.0"));  // not string order
  CHECK(ParseVersion("1.2.3") == ParseVersion("1.2.3"));
}

TEST_CASE("every version round-trips through its own spelling") {
  for (const Version version : {Version{0, 0, 0}, Version{1, 2, 3}, Version{10, 0, 255},
                                Version{4294967295u, 4294967295u, 4294967295u}}) {
    const auto again = ParseVersion(ToString(version));
    REQUIRE(again.has_value());
    CHECK(*again == version);
  }
}
