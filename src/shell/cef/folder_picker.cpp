#include "cef/folder_picker.h"

#include <string>
#include <utility>
#include <vector>

#include "include/wrapper/cef_helpers.h"

namespace sonora::shell {
namespace {

[[nodiscard]] std::filesystem::path Utf8Path(const std::string& utf8) {
  return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

[[nodiscard]] std::string PathToUtf8(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return std::string(text.begin(), text.end());
}

// The dismissal, and the fact that it can arrive after everything it came from
// has gone.
//
// It holds the flag by shared_ptr and the handler by value, and it holds no
// pointer to the picker at all -- so a dialog answered after the window closed
// clears a flag nobody will read again and calls a std::function that owns
// whatever it captured. That is the whole lifetime argument; see the header.
class DismissalHandler final : public CefRunFileDialogCallback {
 public:
  DismissalHandler(std::shared_ptr<bool> open, FolderPicker::Chosen chosen)
      : open_(std::move(open)), chosen_(std::move(chosen)) {}

  void OnFileDialogDismissed(const std::vector<CefString>& file_paths) override {
    CEF_REQUIRE_UI_THREAD();
    if (open_) {
      *open_ = false;
    }
    if (!chosen_) {
      return;
    }

    // Cancelled is an empty list. It is passed on as an empty path rather than
    // swallowed, because "they closed the dialog" and "the dialog never opened"
    // are different facts and only the caller knows which one matters to it.
    std::filesystem::path folder;
    if (!file_paths.empty()) {
      folder = Utf8Path(file_paths.front().ToString());
    }

    // Moved out first: the handler may do anything, including outliving this
    // object, and calling a member that has already been destroyed is the second
    // half of the bug the first half was careful about.
    const FolderPicker::Chosen handler = std::move(chosen_);
    chosen_ = nullptr;
    handler(std::move(folder));
  }

 private:
  std::shared_ptr<bool> open_;
  FolderPicker::Chosen chosen_;

  IMPLEMENT_REFCOUNTING(DismissalHandler);
};

}  // namespace

FolderPicker::FolderPicker() : open_(std::make_shared<bool>(false)) {}

FolderPicker::~FolderPicker() = default;

void FolderPicker::Attach(CefRefPtr<CefBrowser> browser) {
  CEF_REQUIRE_UI_THREAD();
  browser_ = std::move(browser);
}

void FolderPicker::Detach() {
  CEF_REQUIRE_UI_THREAD();
  browser_ = nullptr;
}

bool FolderPicker::Choose(const std::filesystem::path& start, Chosen chosen) {
  CEF_REQUIRE_UI_THREAD();
  if (browser_ == nullptr || *open_ == true) {
    return false;
  }

  *open_ = true;
  browser_->GetHost()->RunFileDialog(
      FILE_DIALOG_OPEN_FOLDER, CefString("Choose the folder Sonora should index"),
      CefString(PathToUtf8(start)),
      // A folder chooser has nothing to filter: the list is empty rather than
      // carrying the audio extensions, which would be a filter on the folders.
      std::vector<CefString>(), new DismissalHandler(open_, std::move(chosen)));
  return true;
}

}  // namespace sonora::shell
