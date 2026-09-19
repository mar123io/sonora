#include <sonora/platform/event_loop.h>

#import <Cocoa/Cocoa.h>

namespace sonora::platform {

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

}  // namespace sonora::platform
