#include <sonora/platform/app_main.h>
#include <sonora/platform/event_loop.h>

#include <utility>

namespace sonora::platform {
namespace {
WorkCallback g_work_callback;
}  // namespace

int RunEventLoop() {
  return 1;
}

void RequestQuit(int exit_code) {
  (void)exit_code;
}

void SetWorkCallback(WorkCallback callback) {
  g_work_callback = std::move(callback);
}

void ScheduleWork(int64_t delay_ms) {
  (void)delay_ms;
}

void* NativeInstanceHandle() noexcept {
  return nullptr;
}

}  // namespace sonora::platform

int main() {
  return sonora::platform::AppMain();
}
