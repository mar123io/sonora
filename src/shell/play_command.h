#pragma once

#include <filesystem>
#include <vector>

namespace sonora::shell {

// `Sonora.exe --play <file> [more files...]`: play a queue to the default
// output device, print what happened, exit. No window, no CEF, no bridge.
//
// It exists because week 5's question -- does this play a whole album side
// without a gap -- is not a question about the user interface, and answering it
// through the interface would mean starting Chromium to find out. This path
// touches the audio engine, the device and nothing else, so when it reports an
// underrun there is one place the fault can be.
//
// With more than one file it is also how week 6's claim gets checked: N tracks
// through one device that is never reopened should produce N-1 gapless joins,
// and the command says how many it actually made.
//
// Returns a process exit code: 0 when everything played to the end with no
// underruns and every join happened, non-zero otherwise, so it can be used
// from a script.
[[nodiscard]] int RunPlayCommand(const std::vector<std::filesystem::path>& paths);

}  // namespace sonora::shell
