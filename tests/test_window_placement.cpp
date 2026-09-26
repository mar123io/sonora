#include <catch2/catch_test_macros.hpp>

#include <vector>

#include <sonora/core/window_placement.h>

using namespace sonora::core;

namespace {

// A 1920x1080 laptop screen with a taskbar along the bottom, at the origin.
[[nodiscard]] Display Laptop() {
  Display display;
  display.work_area = Rect{0, 0, 1920, 1040};
  display.primary = true;
  display.scale = 1.0F;
  return display;
}

// A second monitor to the LEFT of the primary one, which is where negative
// coordinates come from and where "just clamp to zero" goes wrong.
[[nodiscard]] Display MonitorOnTheLeft() {
  Display display;
  display.work_area = Rect{-2560, -200, 2560, 1400};
  display.primary = false;
  display.scale = 1.0F;
  return display;
}

[[nodiscard]] PlacementRequest Request(std::optional<SavedPlacement> saved = std::nullopt) {
  PlacementRequest request;
  request.saved = saved;
  request.default_width = 1100;
  request.default_height = 720;
  request.min_width = 640;
  request.min_height = 480;
  return request;
}

[[nodiscard]] bool Contains(const Rect& outer, const Rect& inner) {
  return inner.x >= outer.x && inner.y >= outer.y && inner.right() <= outer.right() &&
         inner.bottom() <= outer.bottom();
}

}  // namespace

TEST_CASE("the first run is centred on the primary display", "[placement]") {
  const Placement placement = ResolvePlacement(Request(), {MonitorOnTheLeft(), Laptop()});

  REQUIRE(placement.bounds.width == 1100);
  REQUIRE(placement.bounds.height == 720);
  REQUIRE(placement.bounds.x == (1920 - 1100) / 2);
  REQUIRE(placement.bounds.y == (1040 - 720) / 2);
  REQUIRE_FALSE(placement.maximized);
}

TEST_CASE("a saved position that still fits is used exactly", "[placement]") {
  // The whole point of saving it. If this test ever needs a tolerance, the
  // policy has started having opinions it should not have.
  SavedPlacement saved;
  saved.bounds = Rect{300, 120, 1000, 700};
  const Placement placement = ResolvePlacement(Request(saved), {Laptop()});
  REQUIRE(placement.bounds == saved.bounds);
}

TEST_CASE("maximized is remembered", "[placement]") {
  SavedPlacement saved;
  saved.bounds = Rect{300, 120, 1000, 700};
  saved.maximized = true;
  REQUIRE(ResolvePlacement(Request(saved), {Laptop()}).maximized);
}

TEST_CASE("a window saved on a monitor that is gone comes back to the primary", "[placement]") {
  // The unplugged-docking-station case, and the reason this function exists:
  // the saved rectangle is perfectly valid, it just describes a place that no
  // longer has a screen in it.
  SavedPlacement saved;
  saved.bounds = Rect{-1800, 100, 1000, 700};  // on the monitor that is missing

  const Placement placement = ResolvePlacement(Request(saved), {Laptop()});

  REQUIRE(Contains(Laptop().work_area, placement.bounds));
  REQUIRE(placement.bounds.width == 1000);  // the size survives, only the place moves
  REQUIRE(placement.bounds.height == 700);
}

TEST_CASE("a monitor to the left keeps its negative coordinates", "[placement]") {
  // The other half of the same rule. A window really was over there, that
  // screen really is still there, and clamping x to zero would drag it onto
  // the wrong monitor every single start.
  SavedPlacement saved;
  saved.bounds = Rect{-2000, 50, 1200, 800};

  const Placement placement = ResolvePlacement(Request(saved), {Laptop(), MonitorOnTheLeft()});

  REQUIRE(placement.bounds == saved.bounds);
}

TEST_CASE("a window hanging off an edge is slid back in", "[placement]") {
  SavedPlacement saved;
  saved.bounds = Rect{1800, 900, 1000, 700};  // mostly past the right and bottom edges

  const Placement placement = ResolvePlacement(Request(saved), {Laptop()});

  REQUIRE(Contains(Laptop().work_area, placement.bounds));
  REQUIRE(placement.bounds.width == 1000);
  REQUIRE(placement.bounds.height == 700);
  // Slid the minimum distance: flush against the edges it was over.
  REQUIRE(placement.bounds.right() == 1920);
  REQUIRE(placement.bounds.bottom() == 1040);
}

TEST_CASE("a window larger than the screen is clamped to it", "[placement]") {
  // The resolution shrank, or the settings came from a bigger machine.
  SavedPlacement saved;
  saved.bounds = Rect{-100, -100, 4000, 3000};

  const Placement placement = ResolvePlacement(Request(saved), {Laptop()});

  REQUIRE(placement.bounds == Rect{0, 0, 1920, 1040});
}

TEST_CASE("the display is the one the window covers most of", "[placement]") {
  // Straddling both screens, with the top-left corner on the left monitor but
  // most of the window on the laptop. Choosing by corner would send it left.
  SavedPlacement saved;
  saved.bounds = Rect{-200, 100, 1000, 700};

  const Placement placement = ResolvePlacement(Request(saved), {Laptop(), MonitorOnTheLeft()});

  REQUIRE(Contains(Laptop().work_area, placement.bounds));
  REQUIRE(placement.bounds.x == 0);  // slid right out of the left monitor's half
}

TEST_CASE("moving to a display with a different scale keeps the physical size", "[placement]") {
  // The docking-station case that has nothing to do with unplugging: a window
  // saved on a 100% screen, reopened on a 150% one. The same number of pixels
  // there is two thirds of the size, so the number has to change for the window
  // to stay the same.
  Display scaled;
  scaled.work_area = Rect{0, 0, 3840, 2000};
  scaled.primary = true;
  scaled.scale = 1.5F;

  SavedPlacement saved;
  saved.bounds = Rect{100, 100, 1000, 700};
  saved.scale = 1.0F;

  const Placement placement = ResolvePlacement(Request(saved), {scaled});

  REQUIRE(placement.bounds.width == 1500);
  REQUIRE(placement.bounds.height == 1050);

  // And back the other way.
  SavedPlacement from_scaled;
  from_scaled.bounds = Rect{100, 100, 1500, 1050};
  from_scaled.scale = 1.5F;
  const Placement back = ResolvePlacement(Request(from_scaled), {Laptop()});
  REQUIRE(back.bounds.width == 1000);
  REQUIRE(back.bounds.height == 700);
}

TEST_CASE("a corrupt saved placement is ignored, not obeyed", "[placement]") {
  SavedPlacement zero;
  zero.bounds = Rect{10, 10, 0, 0};
  const Placement placement = ResolvePlacement(Request(zero), {Laptop()});
  REQUIRE(placement.bounds.width == 1100);  // the default, centred

  SavedPlacement negative;
  negative.bounds = Rect{10, 10, -500, 700};
  REQUIRE(ResolvePlacement(Request(negative), {Laptop()}).bounds.width == 1100);

  // A scale of zero would divide; NaN would compare false against everything.
  SavedPlacement bad_scale;
  bad_scale.bounds = Rect{100, 100, 1000, 700};
  bad_scale.scale = 0.0F;
  const Placement resolved = ResolvePlacement(Request(bad_scale), {Laptop()});
  REQUIRE(Contains(Laptop().work_area, resolved.bounds));
  REQUIRE(resolved.bounds.width == 1000);
}

TEST_CASE("a window smaller than the minimum is grown", "[placement]") {
  SavedPlacement tiny;
  tiny.bounds = Rect{100, 100, 200, 150};
  const Placement placement = ResolvePlacement(Request(tiny), {Laptop()});
  REQUIRE(placement.bounds.width == 640);
  REQUIRE(placement.bounds.height == 480);
}

TEST_CASE("a screen smaller than the minimum window still gets a visible window",
          "[placement]") {
  // The minimum is a preference; fitting on the screen is not.
  Display small;
  small.work_area = Rect{0, 0, 500, 400};
  small.primary = true;

  SavedPlacement saved;
  saved.bounds = Rect{0, 0, 1000, 700};
  const Placement placement = ResolvePlacement(Request(saved), {small});

  REQUIRE(placement.bounds == Rect{0, 0, 500, 400});
}

TEST_CASE("no displays at all is survivable", "[placement]") {
  const Placement placement = ResolvePlacement(Request(), {});
  REQUIRE(placement.bounds.width == 1100);
  REQUIRE(placement.bounds.height == 720);
}

TEST_CASE("the serialized form survives a round trip", "[placement]") {
  SavedPlacement saved;
  saved.bounds = Rect{-1234, 56, 1000, 700};
  saved.maximized = true;
  saved.scale = 1.5F;

  const auto parsed = ParsePlacement(SerializePlacement(saved));
  REQUIRE(parsed.has_value());
  REQUIRE(parsed->bounds == saved.bounds);
  REQUIRE(parsed->maximized);
  REQUIRE(parsed->scale == 1.5F);
}

TEST_CASE("a settings value written by an older build still parses", "[placement]") {
  // Five numbers, no scale: what the first version of this string looked like.
  const auto parsed = ParsePlacement("100 200 1000 700 0");
  REQUIRE(parsed.has_value());
  REQUIRE(parsed->bounds == Rect{100, 200, 1000, 700});
  REQUIRE(parsed->scale == 1.0F);
}

TEST_CASE("nonsense in the settings table is refused", "[placement]") {
  REQUIRE_FALSE(ParsePlacement("").has_value());
  REQUIRE_FALSE(ParsePlacement("hello").has_value());
  REQUIRE_FALSE(ParsePlacement("100 200").has_value());
  REQUIRE_FALSE(ParsePlacement("100 200 0 0 0").has_value());
  // Numbers big enough that x + width would overflow a signed int, which is
  // undefined behaviour reached from a text file.
  REQUIRE_FALSE(ParsePlacement("2000000000 0 2000000000 1000 0").has_value());
}
