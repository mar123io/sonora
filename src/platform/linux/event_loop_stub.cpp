#include <sonora/platform/event_loop.h>

namespace sonora::platform {

int RunEventLoop() {
  return 1;
}

void RequestQuit(int exit_code) {
  (void)exit_code;
}

}  // namespace sonora::platform
