#include <sonora/core/window_placement.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>

namespace sonora::core {
namespace {

// A scale outside this range is a corrupt settings value, not a display.
constexpr float kMinScale = 0.25F;
constexpr float kMaxScale = 8.0F;

[[nodiscard]] float SaneScale(float scale) noexcept {
  if (!(scale >= kMinScale) || !(scale <= kMaxScale)) {
    // Written as a negated comparison on purpose: it also catches NaN, which a
    // parsed settings value can be and which every ordinary comparison lets
    // through.
    return 1.0F;
  }
  return scale;
}

[[nodiscard]] const Display& PrimaryOf(const std::vector<Display>& displays) {
  for (const Display& display : displays) {
    if (display.primary) {
      return display;
    }
  }
  // No display claimed to be primary. Rather than fail, take the first: the
  // caller has at least one, and any screen the user can see beats none.
  return displays.front();
}

// The display a window belongs to is the one it covers most of. Not the one
// containing its top-left corner, which is the obvious rule and which puts a
// window straddling two monitors on the wrong one whenever its corner is the
// only part on the smaller screen.
[[nodiscard]] const Display& DisplayFor(const Rect& bounds,
                                        const std::vector<Display>& displays) {
  const Display* best = nullptr;
  int best_area = 0;
  for (const Display& display : displays) {
    const int area = IntersectionArea(bounds, display.work_area);
    if (area > best_area) {
      best_area = area;
      best = &display;
    }
  }
  // No overlap with anything: the monitor it was on is gone, or the window was
  // saved off-screen. Either way the primary is where a user will look for it.
  return best != nullptr ? *best : PrimaryOf(displays);
}

// Saturating, because the input is a settings file: a width of two billion is
// not a window, but it must not be undefined behaviour either.
[[nodiscard]] int RoundToInt(float value) noexcept {
  if (!(value > static_cast<float>(std::numeric_limits<int>::min()))) {
    return std::numeric_limits<int>::min();
  }
  if (!(value < static_cast<float>(std::numeric_limits<int>::max()))) {
    return std::numeric_limits<int>::max();
  }
  return static_cast<int>(std::lround(value));
}

}  // namespace

int IntersectionArea(const Rect& a, const Rect& b) noexcept {
  // In 64 bits and clamped on the way out. Two 4K displays multiply to more
  // than a 32-bit product's worth of pixels long before anything unusual
  // happens, and this function only ever has to compare areas.
  const std::int64_t width =
      std::min<std::int64_t>(a.right(), b.right()) - std::max<std::int64_t>(a.x, b.x);
  const std::int64_t height =
      std::min<std::int64_t>(a.bottom(), b.bottom()) - std::max<std::int64_t>(a.y, b.y);
  if (width <= 0 || height <= 0) {
    return 0;
  }
  const std::int64_t area = width * height;
  return static_cast<int>(std::min<std::int64_t>(area, std::numeric_limits<int>::max()));
}

Placement ResolvePlacement(const PlacementRequest& request,
                           const std::vector<Display>& displays) {
  const int default_width = std::max(request.default_width, request.min_width);
  const int default_height = std::max(request.default_height, request.min_height);

  if (displays.empty()) {
    // Nothing to place against. Give the default size at the origin and let the
    // platform put it wherever it puts a window with no position; this is a
    // transient state (a session switch, a display being reconfigured), not a
    // configuration to design for.
    return Placement{Rect{0, 0, default_width, default_height}, false};
  }

  const bool has_saved = request.saved.has_value() && !request.saved->bounds.empty();

  if (!has_saved) {
    // First run. Centred on the primary display rather than at its origin,
    // because a window in the corner of the screen reads as a bug.
    const Rect area = PrimaryOf(displays).work_area;
    const int width = std::min(default_width, area.width);
    const int height = std::min(default_height, area.height);
    return Placement{Rect{area.x + (area.width - width) / 2,
                          area.y + (area.height - height) / 2, width, height},
                     false};
  }

  const SavedPlacement& saved = *request.saved;
  const Display& display = DisplayFor(saved.bounds, displays);

  // The size is remembered in physical pixels, so moving to a display with a
  // different scale has to convert, or the window changes physical size on a
  // machine whose configuration never changed -- a docking station is enough.
  // The position is not converted: it is a point on the virtual desktop, and
  // the same point means the same place whatever the scale there is.
  const float ratio = SaneScale(display.scale) / SaneScale(saved.scale);
  int width = RoundToInt(static_cast<float>(saved.bounds.width) * ratio);
  int height = RoundToInt(static_cast<float>(saved.bounds.height) * ratio);

  const Rect area = display.work_area;
  width = std::clamp(width, std::min(request.min_width, area.width), area.width);
  height = std::clamp(height, std::min(request.min_height, area.height), area.height);

  // Now it fits, so sliding it inside always succeeds. Both clamps are needed
  // and in this order: the first pulls a window back from beyond the right or
  // bottom edge, the second handles a saved position further left or higher
  // than the work area starts -- which is not a corner case on a multi-monitor
  // desktop, where x is routinely negative.
  int x = std::min(saved.bounds.x, area.right() - width);
  int y = std::min(saved.bounds.y, area.bottom() - height);
  x = std::max(x, area.x);
  y = std::max(y, area.y);

  return Placement{Rect{x, y, width, height}, saved.maximized};
}

std::string SerializePlacement(const SavedPlacement& placement) {
  std::ostringstream out;
  out << placement.bounds.x << ' ' << placement.bounds.y << ' ' << placement.bounds.width << ' '
      << placement.bounds.height << ' ' << (placement.maximized ? 1 : 0) << ' '
      << SaneScale(placement.scale);
  return out.str();
}

std::optional<SavedPlacement> ParsePlacement(const std::string& text) {
  std::istringstream in(text);
  SavedPlacement placement;
  int maximized = 0;
  float scale = 1.0F;

  in >> placement.bounds.x >> placement.bounds.y >> placement.bounds.width >>
      placement.bounds.height >> maximized;
  if (in.fail()) {
    return std::nullopt;
  }
  // The scale was added after the first version of this string existed. An
  // older one parses fine and means "the same scale as wherever it opens",
  // which is exactly what a missing value should mean.
  if (in >> scale) {
    placement.scale = SaneScale(scale);
  }

  // A bound on the numbers themselves, not on what they mean. Rect::right()
  // adds x and width, and a settings file that says two billion would make that
  // addition overflow -- undefined behaviour reached from a text file, which is
  // the sort of thing that is much cheaper to refuse here than to survive
  // everywhere downstream. No desktop is a million pixels wide.
  constexpr int kSane = 1000000;
  const Rect& bounds = placement.bounds;
  if (std::abs(bounds.x) > kSane || std::abs(bounds.y) > kSane || bounds.width > kSane ||
      bounds.height > kSane) {
    return std::nullopt;
  }
  if (bounds.empty()) {
    return std::nullopt;
  }
  placement.maximized = maximized != 0;
  return placement;
}

}  // namespace sonora::core
