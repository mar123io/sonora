#pragma once

#include <filesystem>

namespace sonora::shell {

// `Sonora.exe --play <file>`: decode one file to the default output device,
// print what happened, exit. No window, no CEF, no bridge.
//
// It exists because week 5's question -- does this play a whole album side
// without a gap -- is not a question about the user interface, and answering it
// through the interface would mean starting Chromium to find out. This path
// touches the audio engine, the device and nothing else, so when it reports an
// underrun there is one place the fault can be.
//
// Returns a process exit code: 0 when the file played to the end with no
// underruns, non-zero otherwise, so it can be used from a script.
[[nodiscard]] int RunPlayCommand(const std::filesystem::path& path);

}  // namespace sonora::shell
