#pragma once

#include <string>

namespace sonora::platform {

// What registering the scheme did, as one line for the startup log.
enum class SchemeRegistration {
  kUnsupported,   // this platform does not do it here (macOS: Info.plist)
  kAlreadyRight,  // the registry already points at this executable
  kUpdated,       // it pointed somewhere else, or nowhere, and now it does
  kFailed,
};

// Registers this executable as the handler for sonora:// links, for this user.
//
// Per user, never per machine: a per-machine registration needs administrator
// rights, and an application that asks for them to register a URL scheme is an
// application people stop installing. HKEY_CURRENT_USER wins over
// HKEY_LOCAL_MACHINE in the shell's lookup anyway.
//
// Done at every start rather than at install time, because during development
// the executable moves constantly and a stale registration sends a clicked link
// to a build from three weeks ago -- which looks exactly like the link not
// working. Week 10's installer will do the same thing for an installed copy;
// they agree because they write the same value.
SchemeRegistration RegisterUrlScheme(const std::string& scheme,
                                     const std::string& display_name);

[[nodiscard]] std::string ToString(SchemeRegistration result);

}  // namespace sonora::platform
