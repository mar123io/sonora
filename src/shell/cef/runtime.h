#pragma once

#include <filesystem>
#include <string>

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
};

// Entry point of the helper executable: runs one CEF child process (renderer,
// GPU, utility...) to completion and returns its exit code.
int RunChildProcess();

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

}  // namespace sonora::shell
