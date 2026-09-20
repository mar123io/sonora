#include <sonora/platform/paths.h>

#import <Foundation/Foundation.h>
#include <mach-o/dyld.h>

#include <string>
#include <vector>

namespace sonora::platform {

std::filesystem::path ExecutablePath() {
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::vector<char> buffer(size);
  if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
    return {};
  }
  std::error_code ec;
  auto path = std::filesystem::canonical(std::filesystem::path(buffer.data()), ec);
  return ec ? std::filesystem::path(buffer.data()) : path;
}

std::filesystem::path UserDataDirectory(std::string_view app_name) {
  @autoreleasepool {
    NSArray<NSString*>* paths = NSSearchPathForDirectoriesInDomains(
        NSApplicationSupportDirectory, NSUserDomainMask, YES);
    if (paths.count == 0) {
      return std::filesystem::path(".") / std::string(app_name);
    }
    return std::filesystem::path(paths[0].UTF8String) / std::string(app_name);
  }
}

}  // namespace sonora::platform
