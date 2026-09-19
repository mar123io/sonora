#include <sonora/platform/window.h>

#include <cstdio>

// Linux is not a target yet (see ROADMAP.md, phase 3 is Windows-first). This
// stub exists so the tree configures and builds on Linux CI, which is the
// cheapest possible check that nothing platform-specific has crept into core.

namespace sonora::platform {

std::unique_ptr<Window> CreateAppWindow(const WindowDesc& desc) {
  (void)desc;
  std::fprintf(stderr,
               "sonora: no window backend compiled for this platform.\n"
               "        Linux support is not implemented; see ROADMAP.md.\n");
  return nullptr;
}

}  // namespace sonora::platform
