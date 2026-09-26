#include <sonora/platform/window.h>

#import <Cocoa/Cocoa.h>

#include <utility>

// Compile-only target. This file is built by CI on macOS but is not exercised
// on real hardware yet; see the status table in README.md. Its job today is to
// prove that the Window interface is implementable outside Windows, which is
// the whole reason the interface exists.

@interface SonoraWindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic, assign) void* owner;
@end

namespace sonora::platform {
namespace {

class MacWindow;

void NotifyClose(void* owner);

class MacWindow final : public Window {
 public:
  explicit MacWindow(const WindowDesc& desc) {
    const NSRect content = NSMakeRect(0, 0, desc.width_dip, desc.height_dip);
    const NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                    NSWindowStyleMaskMiniaturizable |
                                    NSWindowStyleMaskResizable;

    window_ = [[NSWindow alloc] initWithContentRect:content
                                          styleMask:style
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    window_.title = [NSString stringWithUTF8String:desc.title.c_str()];
    window_.minSize = NSMakeSize(desc.min_width_dip, desc.min_height_dip);
    window_.releasedWhenClosed = NO;
    window_.backgroundColor = [NSColor colorWithCalibratedRed:0.07
                                                        green:0.07
                                                         blue:0.07
                                                        alpha:1.0];
    if (desc.placement.has_value()) {
      // AppKit's origin is the bottom-left of the main screen and y grows
      // upwards; core::Rect is top-left with y growing down, like every other
      // platform this project targets. The flip is the whole of the difference,
      // and it is written here rather than in core because core is the part
      // that must not know which way anybody's y axis points.
      const core::Rect& bounds = desc.placement->bounds;
      const CGFloat screen_height = NSMaxY([NSScreen screens].firstObject.frame);
      const NSRect frame = NSMakeRect(bounds.x, screen_height - bounds.y - bounds.height,
                                      bounds.width, bounds.height);
      [window_ setFrame:frame display:NO];
    } else {
      [window_ center];
    }
    maximize_on_show_ = desc.placement.has_value() && desc.placement->maximized;

    delegate_ = [[SonoraWindowDelegate alloc] init];
    delegate_.owner = this;
    window_.delegate = delegate_;
  }

  ~MacWindow() override {
    window_.delegate = nil;
    [window_ close];
  }

  void Show() override {
    [window_ makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    // "Maximized" has no exact equivalent here -- macOS has zoom, which is a
    // window's idea of its useful size, and full screen, which is a different
    // mode entirely. Zoom is the closer of the two, and the difference is worth
    // knowing about rather than papering over.
    if (maximize_on_show_ && !window_.isZoomed) {
      [window_ zoom:nil];
    }
  }

  void Close() override { [window_ close]; }

  void Raise() override {
    // AppKit has no equivalent of Windows' foreground-stealing rule here:
    // activateIgnoringOtherApps is exactly what it says, and a deminiaturise
    // does the rest.
    if (window_.isMiniaturized) {
      [window_ deminiaturize:nil];
    }
    [window_ makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
  }

  void* native_handle() const noexcept override { return (__bridge void*)window_; }

  SizePx client_size() const noexcept override {
    const NSRect frame = window_.contentView.frame;
    const CGFloat scale = window_.backingScaleFactor;
    return SizePx{static_cast<int>(frame.size.width * scale),
                  static_cast<int>(frame.size.height * scale)};
  }

  float scale_factor() const noexcept override {
    return static_cast<float>(window_.backingScaleFactor);
  }

  void SetOnClose(std::function<void()> handler) override { on_close_ = std::move(handler); }

  void SetOnResize(std::function<void(int, int)> handler) override {
    on_resize_ = std::move(handler);
  }

  core::SavedPlacement SavedPlacement() const override {
    core::SavedPlacement saved;
    saved.scale = scale_factor();
    const NSRect frame = window_.frame;
    const CGFloat screen_height = NSMaxY([NSScreen screens].firstObject.frame);
    saved.bounds = core::Rect{
        static_cast<int>(frame.origin.x), static_cast<int>(screen_height - NSMaxY(frame)),
        static_cast<int>(frame.size.width), static_cast<int>(frame.size.height)};
    saved.maximized = window_.isZoomed;
    return saved;
  }

  void DispatchClose() {
    if (on_close_) {
      on_close_();
    }
  }

  void DispatchResize() {
    if (on_resize_) {
      const NSRect frame = window_.contentView.frame;
      const CGFloat scale = window_.backingScaleFactor;
      on_resize_(static_cast<int>(frame.size.width * scale),
                 static_cast<int>(frame.size.height * scale));
    }
  }

 private:
  NSWindow* window_ = nil;
  SonoraWindowDelegate* delegate_ = nil;
  bool maximize_on_show_ = false;
  std::function<void()> on_close_;
  std::function<void(int, int)> on_resize_;
};

void NotifyClose(void* owner) {
  if (owner != nullptr) {
    static_cast<MacWindow*>(owner)->DispatchClose();
  }
}

void NotifyResize(void* owner) {
  if (owner != nullptr) {
    static_cast<MacWindow*>(owner)->DispatchResize();
  }
}

}  // namespace

std::unique_ptr<Window> CreateAppWindow(const WindowDesc& desc) {
  return std::make_unique<MacWindow>(desc);
}

}  // namespace sonora::platform

@implementation SonoraWindowDelegate

- (BOOL)windowShouldClose:(NSWindow*)sender {
  (void)sender;
  sonora::platform::NotifyClose(self.owner);
  // Mirrors the Win32 behaviour: when a handler is installed the application
  // decides, so never close from here.
  return NO;
}

- (void)windowDidResize:(NSNotification*)notification {
  (void)notification;
  sonora::platform::NotifyResize(self.owner);
}

@end
