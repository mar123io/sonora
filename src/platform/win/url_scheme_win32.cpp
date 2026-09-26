#include <sonora/platform/url_scheme.h>

// clang-format off
#include <windows.h>
// clang-format on

#include <string>

namespace sonora::platform {
namespace {

[[nodiscard]] std::wstring Widen(const std::string& utf8) {
  if (utf8.empty()) {
    return {};
  }
  const int needed =
      ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring wide(static_cast<size_t>(needed), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(),
                        needed);
  return wide;
}

[[nodiscard]] std::wstring ExecutablePathW() {
  std::wstring path(MAX_PATH, L'\0');
  for (;;) {
    const DWORD written =
        ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (written == 0) {
      return {};
    }
    if (written < path.size()) {
      path.resize(written);
      return path;
    }
    path.resize(path.size() * 2);
  }
}

[[nodiscard]] bool WriteString(HKEY key, const wchar_t* name, const std::wstring& value) {
  const auto bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
  return ::RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                          bytes) == ERROR_SUCCESS;
}

[[nodiscard]] std::wstring ReadString(HKEY root,
                                      const std::wstring& subkey,
                                      const wchar_t* name) {
  DWORD bytes = 0;
  if (::RegGetValueW(root, subkey.c_str(), name, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) !=
      ERROR_SUCCESS) {
    return {};
  }
  std::wstring value(bytes / sizeof(wchar_t), L'\0');
  if (::RegGetValueW(root, subkey.c_str(), name, RRF_RT_REG_SZ, nullptr, value.data(),
                     &bytes) != ERROR_SUCCESS) {
    return {};
  }
  // RegGetValueW's byte count includes the terminator, and may include more
  // than one for a value somebody wrote by hand.
  while (!value.empty() && value.back() == L'\0') {
    value.pop_back();
  }
  return value;
}

[[nodiscard]] bool CreateAndWrite(const std::wstring& subkey,
                                  const wchar_t* name,
                                  const std::wstring& value) {
  HKEY key = nullptr;
  if (::RegCreateKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
    return false;
  }
  const bool written = WriteString(key, name, value);
  ::RegCloseKey(key);
  return written;
}

}  // namespace

SchemeRegistration RegisterUrlScheme(const std::string& scheme,
                                     const std::string& display_name) {
  const std::wstring executable = ExecutablePathW();
  if (executable.empty() || scheme.empty()) {
    return SchemeRegistration::kFailed;
  }

  // The quotes around both matter and are the classic bug in this registry
  // entry: without them a path containing a space becomes two arguments, and a
  // %1 containing one becomes two more. %1 is whatever the browser hands over,
  // so it is also where a hostile link would try to inject a second argument --
  // which is why what arrives is parsed by ParseDeepLink and not by anything
  // that would act on a file name.
  const std::wstring command = L"\"" + executable + L"\" \"%1\"";
  const std::wstring root = L"Software\\Classes\\" + Widen(scheme);
  const std::wstring command_key = root + L"\\shell\\open\\command";

  if (ReadString(HKEY_CURRENT_USER, command_key, nullptr) == command) {
    return SchemeRegistration::kAlreadyRight;
  }

  // "URL:<name>" as the default value and an empty "URL Protocol" value are
  // what make the shell treat this key as a protocol rather than a file type.
  // Both are required, and the empty one is easy to leave out because it looks
  // like nothing.
  const bool ok = CreateAndWrite(root, nullptr, L"URL:" + Widen(display_name)) &&
                  CreateAndWrite(root, L"URL Protocol", L"") &&
                  CreateAndWrite(root + L"\\DefaultIcon", nullptr, executable + L",0") &&
                  CreateAndWrite(command_key, nullptr, command);

  return ok ? SchemeRegistration::kUpdated : SchemeRegistration::kFailed;
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
