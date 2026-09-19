#pragma once

namespace sonora::platform {

// Runs the native message loop until RequestQuit is called. Returns the exit
// code passed to RequestQuit.
//
// Week 2 note: CEF wants to drive its own loop. The plan is to keep this
// function as the single entry point and have the CEF integration install
// itself inside it (CefDoMessageLoopWork on a timer, or CefRunMessageLoop
// behind the same signature), so the shell's main() never changes.
int RunEventLoop();

void RequestQuit(int exit_code = 0);

}  // namespace sonora::platform
