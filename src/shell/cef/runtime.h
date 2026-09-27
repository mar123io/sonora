#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace sonora::assets {
class AssetStore;
}  // namespace sonora::assets

namespace sonora::shell {

struct RuntimeConfig {
  void* parent_window = nullptr;  // native handle that hosts the browser view
  int initial_width_px = 0;
  int initial_height_px = 0;
  std::filesystem::path helper_path;
  std::filesystem::path user_data_dir;
  std::string start_url;
  bool enable_devtools = false;
  // Chromium's remote debugging endpoint. 0 disables it entirely.
  int remote_debugging_port = 0;
  const assets::AssetStore* asset_store = nullptr;

  // Where the library index lives. A file, created on first run, safe to delete:
  // it is a cache of what the folder below contains (ADR 0007).
  std::filesystem::path library_path;
  // Folder to index at startup, from --library. Empty leaves the folder alone.
  std::filesystem::path library_root;
  // Rescan the remembered folder at startup. Cheap by design -- an unchanged
  // folder reads no tags at all -- which is what makes it reasonable to do every
  // time rather than only when asked.
  bool scan_at_startup = false;

  // The durable store (ADR 0008): play history, and the stable ids that jump
  // list entries and sonora:// links are made of. Unlike library_path, this one
  // is not safe to delete.
  std::filesystem::path state_path;

  // What the tray menu and an incoming link need from the part of the shell
  // that owns the window. Both may be empty, and then those commands do
  // nothing rather than crash.
  std::function<void()> raise_window;
  std::function<void()> quit;

  // The command line this process was started with, so a sonora:// link in it
  // is acted on once everything exists.
  std::vector<std::string> arguments;

  // "browser", or empty. From --simulate-crash, which main() refuses outside a DevTools
  // build: an executable that will fault on request is a convenience while developing and a
  // liability in a release (ADR 0014).
  //
  // The browser process only, and that is a correction rather than a scope chosen up front.
  // The renderer was going to be crashed by loading chrome://crash, which does nothing at
  // all under CEF: the window stays blank, because CEF serves a short list of chrome://
  // URLs and that is not one of them. The renderer is crashed with the DevTools protocol's
  // Page.crash instead, which needs no code here whatsoever -- the README has the one-liner.
  std::string simulate_crash;
};

// Runs this process as a CEF sub-process if the command line says it is one, and returns the
// exit code it should exit with. Returns **-1** when it is not one -- which is CEF's own way
// of answering the question, and the reason nothing here parses --type= itself.
//
// Both executables call it, and both must call it before anything else they do.
//
// sonora_helper.exe is always a child, so for it this is simply the entry point: renderer,
// GPU and utility processes are launched from it because CefSettings.browser_subprocess_path
// points there.
//
// Sonora.exe is normally the browser process, and it has to call this anyway, because one
// kind of child is launched from it no matter what that setting says: **the crash handler.**
// Chromium starts Crashpad with InitializeCrashpadWithEmbeddedHandler, which re-runs the
// current executable with --type=crashpad-handler. See the call site in main.cpp for what
// that cost when it was missing.
[[nodiscard]] int RunChildProcess();

// Initializes CEF and asks it to create the browser. The browser appears
// asynchronously, once the loop has run: BrowserViewHandle stays null until
// then. Returns false if CEF failed to initialize.
bool StartCef(const RuntimeConfig& config);

void StopCef();

// What this run offers, as one line for the startup log: which capabilities
// are on, which SONORA_DISABLE_CAPS switched off, and which names in it were
// not understood. Valid after StartCef.
[[nodiscard]] std::string CapabilitySummary();

// Native handle of the browser view, or null before it exists.
[[nodiscard]] void* BrowserViewHandle();

// Asks the browser to close. False means there is no browser yet, so the caller
// should quit directly.
bool RequestBrowserClose();

// Acts on a command line that arrived from somewhere else: a second instance
// that handed its arguments over and exited, or this process's own arguments at
// startup. Safe from any thread, and safe before the shell is running -- in
// which case it does nothing, which is the right answer for an activation that
// has nowhere to go yet.
void ActivateWithArguments(const std::vector<std::string>& arguments);

}  // namespace sonora::shell
