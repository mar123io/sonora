#pragma once

#include <filesystem>
#include <functional>
#include <memory>

#include "include/cef_browser.h"

namespace sonora::shell {

// The one place a filesystem path enters this program from the person using it.
//
// Week 7 took paths off the bridge in the other direction: the page hands out
// track ids and never sees a path, because a page that can name a file is a page
// that can be talked into naming the wrong one. This is the exception that proves
// the rule -- the path is chosen in a dialog the operating system draws, it is
// produced by the shell rather than sent by the page, and the page is never told
// what it was except as the `root` field of a status it was already being sent.
//
// Everything here runs on the CEF UI thread, which is what makes the plain
// members below safe without a lock. The dialog itself does not: RunFileDialog
// returns immediately and answers later, and that is not an inconvenience to work
// around but the only acceptable shape. Bridge calls are served one after another
// on this thread, so a handler that waited for a person to find a folder would
// hold up every call behind it -- the interface would freeze, and the timeout the
// page eventually reported would name whichever call happened to be waiting
// rather than this one.
class FolderPicker {
 public:
  // The folder the person chose, or an empty path when they cancelled.
  // Cancelling is an answer, not a failure, and the caller gets to see it.
  using Chosen = std::function<void(std::filesystem::path)>;

  FolderPicker();
  ~FolderPicker();

  FolderPicker(const FolderPicker&) = delete;
  FolderPicker& operator=(const FolderPicker&) = delete;

  // Called from SonoraClient when a browser appears and when it goes, the same
  // way the event channel is: the client is what knows whether there is a window
  // to hang a modal dialog on.
  void Attach(CefRefPtr<CefBrowser> browser);
  void Detach();

  // False when there is no browser, or when a chooser is already open. Both are
  // ordinary answers: a second dialog stacked on the first is the thing this
  // returns false to prevent, and it is exactly what a double-click on the button
  // would otherwise produce.
  bool Choose(const std::filesystem::path& start, Chosen chosen);

 private:
  CefRefPtr<CefBrowser> browser_;

  // Shared with whichever callback is in flight, rather than owned outright.
  //
  // The callback is reference counted by CEF and outlives this object in at least
  // one real case: the window is closed while the chooser is open, the browser
  // goes, the runtime tears down, and the dismissal still arrives. A raw `this`
  // in that callback is a use-after-free with a dialog in front of it; a shared
  // flag is a write to memory that is still there.
  std::shared_ptr<bool> open_;
};

}  // namespace sonora::shell
