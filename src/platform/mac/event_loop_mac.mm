#include <sonora/platform/app_main.h>
#include <sonora/platform/event_loop.h>

#import <Cocoa/Cocoa.h>

#include <utility>

namespace sonora::platform {
namespace {
WorkCallback g_work_callback;
}  // namespace

int RunEventLoop() {
  @autoreleasepool {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [NSApp run];
  }
  return 0;
}

void RequestQuit(int exit_code) {
  (void)exit_code;  // AppKit owns the process exit code.
  [NSApp stop:nil];
  // -stop: only takes effect after the next event, so post a dummy one.
  NSEvent* wake = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                     location:NSZeroPoint
                                modifierFlags:0
                                    timestamp:0
                                 windowNumber:0
                                      context:nil
                                      subtype:0
                                        data1:0
                                        data2:0];
  [NSApp postEvent:wake atStart:YES];
}

void SetWorkCallback(WorkCallback callback) {
  g_work_callback = std::move(callback);
}

void ScheduleWork(int64_t delay_ms) {
  if (!g_work_callback) {
    return;
  }
  // dispatch_after on the main queue is the AppKit equivalent of the Win32
  // message-only window: it runs the block from inside the existing run loop.
  const dispatch_time_t when =
      dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(delay_ms) * NSEC_PER_MSEC);
  dispatch_after(when, dispatch_get_main_queue(), ^{
    if (g_work_callback) {
      g_work_callback();
    }
  });
}

void* NativeInstanceHandle() noexcept {
  return nullptr;
}

}  // namespace sonora::platform

int main() {
  return sonora::platform::AppMain();
}
