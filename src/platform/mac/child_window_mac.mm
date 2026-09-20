#include <sonora/platform/child_window.h>

#import <Cocoa/Cocoa.h>

namespace sonora::platform {

void SetChildWindowBounds(void* native_child, int x, int y, int width, int height) {
  NSView* view = (__bridge NSView*)native_child;
  if (view == nil) {
    return;
  }
  // Cocoa takes points, not pixels, and its origin is bottom-left. The caller
  // works in top-left physical pixels, which is what CEF and Win32 use.
  const CGFloat scale = view.window != nil ? view.window.backingScaleFactor : 1.0;
  const CGFloat parent_height = view.superview != nil ? view.superview.frame.size.height : 0;
  const CGFloat w = width / scale;
  const CGFloat h = height / scale;
  view.frame = NSMakeRect(x / scale, parent_height - (y / scale) - h, w, h);
}

}  // namespace sonora::platform
