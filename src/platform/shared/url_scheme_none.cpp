#include <sonora/platform/url_scheme.h>

// Registering a URL scheme is not something a program does at runtime
// everywhere.
//
// On macOS it is a declaration in the bundle's Info.plist, read by Launch
// Services when the bundle is installed -- there is no call to make, and a
// call that pretended to make one would be a lie with a return value. On Linux
// it is a .desktop file with an x-scheme-handler MIME type, which belongs to
// whatever installs the application, not to the application.
//
// So this returns kUnsupported, the shell prints it, and nobody spends an
// afternoon wondering why a link does nothing.

namespace sonora::platform {

SchemeRegistration RegisterUrlScheme(const std::string& scheme,
                                     const std::string& display_name) {
  (void)scheme;
  (void)display_name;
  return SchemeRegistration::kUnsupported;
}

std::string ToString(SchemeRegistration result) {
  switch (result) {
    case SchemeRegistration::kUnsupported:
      return "not registered on this platform";
    case SchemeRegistration::kAlreadyRight:
      return "already pointing here";
    case SchemeRegistration::kUpdated:
      return "registered for this user";
    case SchemeRegistration::kFailed:
      break;
  }
  return "failed";
}

}  // namespace sonora::platform
