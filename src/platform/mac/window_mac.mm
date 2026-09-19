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
    [window_ center];

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
  }

  void Close() override { [window_ close]; }

  void* native_handle() const noexcept override { return (__bridge void*)window_; }

  float scale_factor() const noexcept override {
    return static_cast<float>(window_.backingScaleFactor);
  }

  void SetOnClose(std::function<void()> handler) override { on_close_ = std::move(handler); }

  void SetOnResize(std::function<void(int, int)> handler) override {
    on_resize_ = std::move(handler);
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
