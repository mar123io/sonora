// The CEF helper executable.
//
// CEF runs its renderer, GPU and utility work in child processes. They can be
// launched from the main executable, but a dedicated helper is better: the main
// binary can keep its manifest, its icon and eventually its own startup work
// without any of it running dozens of times per session, and the helper stays
// small enough that spawning one is cheap.

#include <sonora/platform/app_main.h>

#include "cef/runtime.h"

int sonora::platform::AppMain() {
  return sonora::shell::RunChildProcess();
}
