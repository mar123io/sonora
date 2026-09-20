#pragma once

#include <cstdint>
#include <functional>

namespace sonora::platform {

// Runs the native message loop until RequestQuit is called. Returns the exit
// code passed to RequestQuit.
int RunEventLoop();

void RequestQuit(int exit_code = 0);

// ---------------------------------------------------------------------------
// External work pump
// ---------------------------------------------------------------------------
// CEF can run with an external message pump: instead of owning the loop, it
// asks the host to call back after a delay and does its work there. That keeps
// a single native loop for the whole application, which is what makes native
// menus, media keys and modal dialogs behave.
//
// The platform layer owns the mechanics (a message-only window and a timer on
// Windows) and knows nothing about CEF; the CEF layer registers a callback and
// asks for it to be run. See docs/adr/0003.
using WorkCallback = std::function<void()>;

// Must be called on the loop thread, before the first ScheduleWork.
void SetWorkCallback(WorkCallback callback);

// Requests that the work callback run after delay_ms (0 = as soon as the loop
// is idle). Safe to call from any thread. A later call replaces any pending
// request rather than queueing a second one.
void ScheduleWork(int64_t delay_ms);

}  // namespace sonora::platform
