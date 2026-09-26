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
// The third party to the agreement does not exist yet: week 10's installer has
// to put this exact string on the shortcut it writes, or the shell will treat
// the installed application and the running one as two different programs. That
// is also the reason the media panel still says "Unknown app" when Sonora is
// launched from a build folder: the process claims an identity that nothing has
// registered, and the shell believes the registration, not the claim.
//
// Form: CompanyName.ProductName, as documented, and stable forever -- changing
// it orphans every jump list and every pinned shortcut that carries the old one.

// clang-format off
#include <windows.h>
#include <shlobj.h>
// clang-format on

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

}  // namespace sonora::platform
