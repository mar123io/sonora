#include <sonora/platform/single_instance.h>

// clang-format off
#include <windows.h>
// clang-format on

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace sonora::platform {
namespace {

constexpr wchar_t kListenerClassName[] = L"SonoraSingleInstanceListener";

// The arguments travel as one UTF-8 blob with '\n' between them. A separator
// that cannot occur in a Windows path or in a sonora:// URL, and one byte
// rather than a serialisation format: what crosses here is a command line, not
// a document.
constexpr char kArgumentSeparator = '\n';

// WM_COPYDATA carries an arbitrary number the receiver can use to tell messages
// apart. Any process can send one, so this is a sanity check and not a
// permission -- see the comment in the window procedure.
constexpr ULONG_PTR kActivationMessageId = 0x50'4E'53'01;  // "SNP\1"

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

[[nodiscard]] std::string Join(const std::vector<std::string>& arguments) {
  std::string joined;
  for (const std::string& argument : arguments) {
    if (!joined.empty()) {
      joined += kArgumentSeparator;
    }
    joined += argument;
  }
  return joined;
}

[[nodiscard]] std::vector<std::string> Split(const char* data, std::size_t size) {
  std::vector<std::string> arguments;
  std::string current;
  for (std::size_t i = 0; i < size; ++i) {
    if (data[i] == kArgumentSeparator) {
      arguments.push_back(std::move(current));
      current.clear();
      continue;
    }
    if (data[i] == '\0') {
      break;  // a sender that included its terminator
    }
    current.push_back(data[i]);
  }
  if (!current.empty()) {
    arguments.push_back(std::move(current));
  }
  return arguments;
}

class Win32SingleInstance final : public SingleInstance {
 public:
  Win32SingleInstance(const std::string& id, ActivationFn on_activation)
      : on_activation_(std::move(on_activation)) {
    // Local\ rather than Global\: the claim is per session, so two users logged
    // into the same machine each get their own Sonora. A global mutex would
    // mean the second user's launch silently raising a window on somebody
    // else's desktop, which is worse than a second process.
    const std::wstring base = L"Local\\" + Widen(id) + L".SingleInstance";
    mutex_ = ::CreateMutexW(nullptr, TRUE, base.c_str());
    const DWORD error = ::GetLastError();
    primary_ = mutex_ != nullptr && error != ERROR_ALREADY_EXISTS;

    window_title_ = Widen(id);
    handoff_name_ = base + L".Window";

    if (primary_) {
      CreateListenerWindow();
      PublishListener();
    }
    std::printf("single instance: %s, listener %p\n", primary_ ? "primary" : "secondary",
                static_cast<void*>(listener_));
  }

  ~Win32SingleInstance() override {
    if (handoff_view_ != nullptr) {
      ::UnmapViewOfFile(handoff_view_);
      handoff_view_ = nullptr;
    }
    if (handoff_ != nullptr) {
      ::CloseHandle(handoff_);
      handoff_ = nullptr;
    }
    if (listener_ != nullptr) {
      ::SetWindowLongPtrW(listener_, GWLP_USERDATA, 0);
      ::DestroyWindow(listener_);
    }
    if (mutex_ != nullptr) {
      // Released explicitly: a mutex abandoned by a crash is still signalled to
      // the next waiter, but releasing it here means the next launch after a
      // clean exit does not depend on that subtlety.
      ::ReleaseMutex(mutex_);
      ::CloseHandle(mutex_);
    }
  }

  [[nodiscard]] bool IsPrimary() const noexcept override { return primary_; }

  [[nodiscard]] bool ForwardToPrimary(const std::vector<std::string>& arguments) override {
    // Retried rather than asked once. Losing this race is not hypothetical: the
    // claim is taken before the listener exists, so a launch that lands in
    // between finds a mutex with nobody behind it -- and the honest reading of
    // that is "the first copy is still starting", not "it is gone". Two seconds
    // of patience here beats a second process fighting the first one for the
    // user data directory, which is how this failure actually shows up.
    constexpr int kAttempts = 40;
    constexpr DWORD kWaitMs = 50;

    HWND target = nullptr;
    for (int attempt = 0; attempt < kAttempts && target == nullptr; ++attempt) {
      if (attempt > 0) {
        ::Sleep(kWaitMs);
      }
      target = FindListener();
    }

    if (target == nullptr) {
      std::printf("single instance: no listener after %d attempts\n", kAttempts);
      return false;
    }

    // An activation with nothing to say is the common case, not a malformed
    // one: somebody double-clicked Sonora while Sonora was already running, and
    // what they are asking for is the window they already have. The payload is
    // then empty, and WM_COPYDATA wants a null pointer when the length is zero.
    const std::string joined = Join(arguments);
    COPYDATASTRUCT payload{};
    payload.dwData = kActivationMessageId;
    payload.cbData = static_cast<DWORD>(joined.size());
    payload.lpData = joined.empty() ? nullptr : const_cast<char*>(joined.data());

    // SendMessageTimeout, not SendMessage: the other process may be wedged, and
    // a launcher that hangs forever because the running copy is busy is a worse
    // failure than a second instance. Two seconds is long enough for a message
    // loop that is merely slow.
    DWORD_PTR result = 0;
    const LRESULT sent = ::SendMessageTimeoutW(
        target, WM_COPYDATA, reinterpret_cast<WPARAM>(nullptr),
        reinterpret_cast<LPARAM>(&payload), SMTO_ABORTIFHUNG, 2000, &result);

    const bool delivered = sent != 0 && result != 0;
    if (!delivered) {
      // Three different failures used to look alike here. They do not any more:
      // sent == 0 is a listener that did not answer in time, result == 0 is one
      // that answered and refused.
      std::printf("single instance: listener %p %s\n", static_cast<void*>(target),
                  sent == 0 ? "did not answer in time" : "refused the message");
    }
    return delivered;
  }

 private:
  // The primary writes its window handle into a named shared page; the
  // secondary reads it.
  //
  // FindWindowEx(HWND_MESSAGE, ...) is the documented way to find a
  // message-only window and it is what this used to rely on alone. It is also
  // the part that failed first on a real machine, with no error to read: a
  // lookup by class name and title has three things that must match and tells
  // you nothing about which one did not. A handle written by the process that
  // owns it has none -- either the page is there or it is not, and the
  // difference is visible from both sides.
  void PublishListener() {
    if (listener_ == nullptr) {
      return;
    }
    handoff_ = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                    sizeof(std::uint64_t), handoff_name_.c_str());
    if (handoff_ == nullptr) {
      return;
    }
    // Kept mapped and open for the life of the process: a file mapping exists
    // only while somebody holds a handle to it, so closing this would delete
    // the answer as soon as it was written.
    void* view = ::MapViewOfFile(handoff_, FILE_MAP_WRITE, 0, 0, sizeof(std::uint64_t));
    if (view == nullptr) {
      return;
    }
    handoff_view_ = view;
    *static_cast<std::uint64_t*>(view) =
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(listener_));
  }

  [[nodiscard]] HWND FindListener() const {
    HANDLE mapping = ::OpenFileMappingW(FILE_MAP_READ, FALSE, handoff_name_.c_str());
    if (mapping != nullptr) {
      void* view = ::MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(std::uint64_t));
      HWND published = nullptr;
      if (view != nullptr) {
        published = reinterpret_cast<HWND>(
            static_cast<std::uintptr_t>(*static_cast<const std::uint64_t*>(view)));
        ::UnmapViewOfFile(view);
      }
      ::CloseHandle(mapping);
      // IsWindow, because a handle written by a process that has since died is
      // a number that still looks like a handle -- and handles are reused.
      if (published != nullptr && ::IsWindow(published) != 0) {
        return published;
      }
    }

    // The older path, kept as a fallback: it costs one call and it is what
    // keeps a mixed pair of builds working during development.
    HWND found =
        ::FindWindowExW(HWND_MESSAGE, nullptr, kListenerClassName, window_title_.c_str());
    if (found == nullptr) {
      found = ::FindWindowW(kListenerClassName, window_title_.c_str());
    }
    return found;
  }

  void CreateListenerWindow() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &Win32SingleInstance::WndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = kListenerClassName;
    ::RegisterClassExW(&wc);  // fails harmlessly if already registered

    // HWND_MESSAGE: no pixels, no taskbar entry, no broadcast messages. It
    // exists to receive one message from one other process.
    listener_ = ::CreateWindowExW(0, kListenerClassName, window_title_.c_str(), 0, 0, 0, 0, 0,
                                  HWND_MESSAGE, nullptr, wc.hInstance, this);
  }

  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_NCCREATE) {
      auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
      ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }

    auto* self =
        reinterpret_cast<Win32SingleInstance*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self != nullptr && msg == WM_COPYDATA) {
      return self->HandleCopyData(reinterpret_cast<const COPYDATASTRUCT*>(lparam));
    }
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }

  [[nodiscard]] LRESULT HandleCopyData(const COPYDATASTRUCT* payload) const {
    // Everything here is treated as hostile input, because it is: any process
    // on this desktop can send a WM_COPYDATA to any window it can find, and
    // this window is findable by design. The identifier is a filter, not a
    // credential; the length is bounded; and what comes out is a list of
    // strings that still has to survive ParseDeepLink before it means
    // anything -- which accepts decimal digits and refuses everything else.
    if (payload == nullptr || payload->dwData != kActivationMessageId) {
      return 0;
    }
    // 64 KiB of command line is already absurd; the real ones are under 200
    // bytes. A bound here is what stops a hostile sender from making this
    // process allocate whatever it likes.
    constexpr DWORD kMaxPayload = 64 * 1024;
    if (payload->cbData > kMaxPayload) {
      return 0;
    }

    // An empty payload is an activation with no arguments, and it still means
    // something: raise the window. Rejecting it as malformed is what made a
    // plain second launch report that the first copy "did not answer" -- the
    // message arrived, was understood, and was thrown away for being quiet.
    std::vector<std::string> arguments;
    if (payload->lpData != nullptr && payload->cbData > 0) {
      arguments = Split(static_cast<const char*>(payload->lpData), payload->cbData);
    }
    if (on_activation_) {
      on_activation_(arguments);
    }
    // Non-zero so the sender can tell the difference between "delivered" and
    // "nobody was there", which is what decides whether it exits or carries on.
    return 1;
  }

  ActivationFn on_activation_;
  HANDLE mutex_ = nullptr;
  HANDLE handoff_ = nullptr;
  void* handoff_view_ = nullptr;
  HWND listener_ = nullptr;
  std::wstring window_title_;
  std::wstring handoff_name_;
  bool primary_ = false;
};

}  // namespace

std::unique_ptr<SingleInstance> ClaimSingleInstance(const std::string& id,
                                                    ActivationFn on_activation) {
  return std::make_unique<Win32SingleInstance>(id, std::move(on_activation));
}

}  // namespace sonora::platform
