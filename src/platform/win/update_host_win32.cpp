#include "sonora/platform/update_host.h"

#include <windows.h>

#include <system_error>
#include <vector>

namespace sonora::platform {
namespace {

// A HANDLE that closes itself. There are four of them in this file and every one of
// them is a leak waiting for an early return.
class Handle {
 public:
  explicit Handle(HANDLE handle) : handle_(handle) {}
  ~Handle() {
    if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
      ::CloseHandle(handle_);
    }
  }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;

  [[nodiscard]] bool ok() const noexcept {
    return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
  }
  [[nodiscard]] HANDLE get() const noexcept { return handle_; }

 private:
  HANDLE handle_;
};

std::optional<std::filesystem::path> TemporaryDirectory() {
  std::vector<wchar_t> buffer(MAX_PATH + 1);
  DWORD written = ::GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
  if (written > buffer.size()) {
    buffer.resize(written + 1);
    written = ::GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
  }
  if (written == 0) {
    return std::nullopt;
  }
  return std::filesystem::path(std::wstring(buffer.data(), written));
}

// Quoting for a command line, which on Windows is one string that every program parses
// for itself. These are the rules CommandLineToArgvW documents, and the only reason
// this is here rather than being avoided is that CreateProcessW has no argv form.
std::wstring Quote(const std::wstring& argument) {
  if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
    return argument;
  }
  std::wstring quoted;
  quoted.push_back(L'"');
  for (std::size_t i = 0; i < argument.size(); ++i) {
    std::size_t backslashes = 0;
    while (i < argument.size() && argument[i] == L'\\') {
      ++i;
      ++backslashes;
    }
    if (i == argument.size()) {
      quoted.append(backslashes * 2, L'\\');
      break;
    }
    if (argument[i] == L'"') {
      quoted.append(backslashes * 2 + 1, L'\\');
    } else {
      quoted.append(backslashes, L'\\');
    }
    quoted.push_back(argument[i]);
  }
  quoted.push_back(L'"');
  return quoted;
}

std::wstring Widen(std::string_view text) {
  if (text.empty()) {
    return {};
  }
  const int needed =
      ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  if (needed <= 0) {
    return {};
  }
  std::wstring wide(static_cast<std::size_t>(needed), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(),
                        needed);
  return wide;
}

}  // namespace

std::optional<std::filesystem::path> CopyExecutableToTemporary(std::string_view stem) {
  const std::filesystem::path self = ExecutablePath();
  const auto temporary = TemporaryDirectory();
  if (self.empty() || !temporary.has_value()) {
    return std::nullopt;
  }
  // The process id in the name so that two of these cannot collide, and so that a
  // leftover from a crash is identifiable rather than mysterious.
  const std::wstring name =
      Widen(stem) + L"-" + std::to_wstring(::GetCurrentProcessId()) + L".exe";
  const std::filesystem::path destination = *temporary / name;
  if (::CopyFileW(self.c_str(), destination.c_str(), FALSE) == 0) {
    return std::nullopt;
  }
  return destination;
}

bool SpawnDetached(const std::filesystem::path& executable,
                   const std::vector<std::string>& arguments) {
  std::wstring command_line = Quote(executable.wstring());
  for (const std::string& argument : arguments) {
    command_line.push_back(L' ');
    command_line.append(Quote(Widen(argument)));
  }

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};

  // CREATE_BREAKAWAY_FROM_JOB is deliberately not here. A child that breaks out of the
  // job object would survive the application being killed, and the one thing this
  // process must not do is outlive the thing it is waiting for in a way nobody can see.
  const BOOL started = ::CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE,
                                        CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr,
                                        executable.parent_path().c_str(), &startup, &process);
  if (started == 0) {
    return false;
  }
  ::CloseHandle(process.hThread);
  ::CloseHandle(process.hProcess);
  return true;
}

std::uint32_t CurrentProcessId() {
  return ::GetCurrentProcessId();
}

bool WaitForProcess(std::uint32_t process_id, int timeout_ms) {
  const Handle process(::OpenProcess(SYNCHRONIZE, FALSE, process_id));
  if (!process.ok()) {
    // Already gone, or not ours to wait for. Both mean "do not wait", and the common
    // case by far is the first: the updater is started as the application exits.
    return true;
  }
  const DWORD result = ::WaitForSingleObject(
      process.get(), timeout_ms < 0 ? INFINITE : static_cast<DWORD>(timeout_ms));
  return result == WAIT_OBJECT_0;
}

sonora::update::FlushFn DurableFlush() {
  return [](const std::filesystem::path& path) {
    // Reopening the file that was just closed, for write, and asking Windows to put it
    // on the disk. FILE_FLAG_NO_BUFFERING is not what is wanted here -- that changes how
    // the writes happened, and they have already happened. FlushFileBuffers is the one
    // that says "and now really".
    const Handle file(::CreateFileW(path.c_str(), GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.ok()) {
      return false;
    }
    return ::FlushFileBuffers(file.get()) != 0;
  };
}

}  // namespace sonora::platform
