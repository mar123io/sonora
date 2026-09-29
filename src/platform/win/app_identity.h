#pragma once

// Who this program is, as far as the Windows shell is concerned -- in one
// place, because three things have to agree about it and two of them are in
// other files.
//
// The AppUserModelID groups taskbar buttons, names the jump list's storage, and
// is what a Start Menu shortcut carries so the shell can tie a running process
// to an installed application. Week 8 removed the call that set it, and
// correctly: nothing registered it then, and an id nobody claims is a promise
// with no keeper. Week 9 is what changed that -- a jump list is stored *per
// AppUserModelID*, so from here on something does claim it.
//
// The third party to the agreement is week 10's installer, which puts this exact
// string on the shortcut it writes -- without that the shell treats the installed
// application and the running one as two different programs.
//
// A shortcut is not the only registration, and for a long time it was the only
// one here, which is why the media panel said "Unknown app" over a playing track
// on any build run out of its own folder: the process claimed an identity that
// nothing had registered, and the shell believes the registration and not the
// claim. RegisterAppUserModelId below is the registration that does not depend on
// having been installed.
//
// Form: CompanyName.ProductName, as documented, and stable forever -- changing
// it orphans every jump list and every pinned shortcut that carries the old one.

// clang-format off
#include <windows.h>
#include <shlobj.h>
// clang-format on

#include <string>
#include <vector>

namespace sonora::platform {

inline constexpr wchar_t kAppUserModelId[] = L"MarioLizzio.Sonora";

// Called before the first window is created: the shell reads the id when it
// creates the taskbar button, and setting it afterwards leaves that button
// grouped under whatever it guessed in the meantime.
//
// Idempotent and cheap; calling it twice is allowed and the second call is
// what makes the ordering above nobody's secret.
inline HRESULT SetProcessAppUserModelId() {
  static const HRESULT result = ::SetCurrentProcessExplicitAppUserModelID(kAppUserModelId);
  return result;
}

// Gives the id a name, so that the things the shell draws from it say "Sonora".
//
// This is the other half of the promise the comment above describes, and the
// reason the media panel said "App sconosciuta" over a playing track: the process
// claimed MarioLizzio.Sonora, the media controls asked the shell what application
// that is, and the shell -- which believes its own registrations and not a
// process's claim about itself -- had nothing to answer with. The panel then
// falls back to a string that is not wrong so much as unhelpful.
//
// The Start Menu shortcut the installer writes carries the same id and is one way
// to register it. It is not enough on its own, because it only exists once Sonora
// has been installed, and a build run out of its own folder is not a strange case
// -- it is the case every developer sees and the one the panel was photographed
// in. The class registration below covers both, and it is where the shell looks
// for an unpackaged application's display name and icon.
//
// HKCU, not HKLM: this is the current user's view of who this application is, it
// needs no elevation, and the installer registers the same values so an uninstall
// takes them away again. Idempotent, best effort, and nothing depends on it
// working -- the failure mode is the string that was there before.
namespace detail {

[[nodiscard]] inline std::wstring ThisExecutablePath() {
  std::vector<wchar_t> path(MAX_PATH);
  for (;;) {
    const DWORD written =
        ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (written == 0) {
      return {};
    }
    if (written < path.size()) {
      return std::wstring(path.data(), written);
    }
    path.resize(path.size() * 2);
  }
}

inline bool WriteIdentityValue(HKEY key, const wchar_t* name, const std::wstring& value) {
  const auto bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
  return ::RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                          bytes) == ERROR_SUCCESS;
}

}  // namespace detail

inline bool RegisterAppUserModelId() {
  static const bool registered = [] {
    HKEY key = nullptr;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER,
                          L"Software\\Classes\\AppUserModelId\\MarioLizzio.Sonora", 0, nullptr,
                          REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &key,
                          nullptr) != ERROR_SUCCESS) {
      return false;
    }
    bool ok = detail::WriteIdentityValue(key, L"DisplayName", L"Sonora");
    // The icon the panel draws next to the name. The executable carries it as a
    // resource, so the path to the executable is the whole answer -- and it is
    // read at display time, which is why it is written every run rather than
    // once: a build folder moves, an installed copy does not, and the last one to
    // run is the one that is there.
    if (const std::wstring executable = detail::ThisExecutablePath(); !executable.empty()) {
      ok = detail::WriteIdentityValue(key, L"IconUri", executable) && ok;
    }
    ::RegCloseKey(key);
    return ok;
  }();
  return registered;
}

}  // namespace sonora::platform
