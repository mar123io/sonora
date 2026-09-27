#pragma once

#include <filesystem>
#include <string>

// filestore.h and not journal.h: Layout lives there, because it is about where the four
// directories are rather than about what is in flight between them. journal.h arrives
// with it.
#include <sonora/update/filestore.h>

namespace sonora::shell {

// The application's four dealings with the updater, and there are only four.
//
// Everything that decides anything is in sonora::update and everything that moves a
// directory is in sonora-updater.exe. This file is the wiring, and it is short on
// purpose: the shell's part in an update is to notice, to get out of the way, and to say
// when it started.
//
//   1. RecoverBeforeStartup(), in main() before CEF exists. Finishes or undoes whatever
//      the journal describes. If the next step would move the directory this process was
//      loaded from, it starts the out-of-tree updater and asks main() to exit.
//   2. StartPeriodicCheck(), after the window is up. Runs sonora-updater --check now and
//      then every six hours, in a separate process that exits.
//   3. NotifyUiLoaded(), from the load handler. The milestone ADR 0011 uses instead of a
//      twenty-second timer: the window is up and the page has finished loading, which is
//      the most this process can honestly claim about whether it works.
//   4. ApplyStagedUpdateAtExit(), last thing in main(). If something is staged, hands it
//      to the out-of-tree updater and returns; the updater waits for this process to go.

// Where the four directories of ADR 0011 are, derived from where this executable is.
//
// In an installed copy that is %LOCALAPPDATA%\Programs, with the installation called
// "Sonora". In a development build it is build/<preset>, with the installation called
// "bin" -- which is harmless and deliberately not special-cased: the journal lands in
// build/<preset>/bin.update, nothing is ever staged because a development build's version
// is not a release version, and the alternative is a code path that only runs on a
// developer's machine.
[[nodiscard]] update::Layout UpdateLayout();

enum class StartupAction {
  kContinue,  // nothing to do, or it is done: carry on and start CEF
  kHandOff,   // the updater is now running out of tree; this process must exit
};

[[nodiscard]] StartupAction RecoverBeforeStartup();

void StartPeriodicCheck();

// Idempotent, and called on the CEF UI thread. The flag is written once per run: a page
// that reloads has not made the installation any more or less able to start.
void NotifyUiLoaded();

void ApplyStagedUpdateAtExit();

// For the about panel: what the updater last refused, so that an installation that has
// stopped taking updates can say why. See ADR 0011 -- a refusal the user cannot see is
// indistinguishable from an updater that is broken.
[[nodiscard]] std::string UpdateStatusSummary();

}  // namespace sonora::shell
