#pragma once

#include <string_view>

#include "sonora/update/journal.h"

namespace sonora::update {

// The loop around NextStep.
//
// It is four lines long and it is in the library rather than in the updater
// executable for one reason: the ordering in it -- write the journal, *then* act --
// is the whole crash-safety argument, and a four-line loop that lives next to the
// Win32 calls is a four-line loop nobody ever tests. Here, the tests drive it with
// a filesystem made of three booleans and stop it after every operation it is
// capable of performing.
//
// Everything that touches a disk is behind Runner, which has three methods.

class Runner {
 public:
  virtual ~Runner();

  // Durably. The journal is the only record of what is in flight, so a write that
  // is reported as successful and is not on the disk afterwards is the one failure
  // this design cannot survive -- which is why the Win32 implementation writes a
  // temporary file, flushes it, and renames it over the old one.
  [[nodiscard]] virtual bool WriteJournal(const Journal& journal) = 0;

  [[nodiscard]] virtual Facts Look() = 0;

  [[nodiscard]] virtual bool Perform(Step step) = 0;
};

enum class DriveResult {
  kDone,                // nothing left to do
  kNeedsOutOfProcess,   // the next step moves the tree this process is running from
  kJournalWriteFailed,  // nothing was done; the installation is untouched
  kStepFailed,          // the journal records the intent, so the next start retries
  kStalled,             // the bound was hit: a state this version cannot resolve
};

[[nodiscard]] std::string_view Describe(DriveResult result);

// Runs steps until there are none left.
//
// `may_touch_installation` is false in Sonora.exe, which reads the journal at
// startup but must never move a directory it was loaded from; it returns
// kNeedsOutOfProcess instead, and the caller launches the out-of-tree copy of the
// updater and exits. It is true in that copy.
//
// `journal` is updated in place so that the caller can report what happened -- and
// so that a kStepFailed leaves the caller holding the journal that is on the disk,
// not the one it started with.
[[nodiscard]] DriveResult Drive(Journal& journal, Runner& runner, bool may_touch_installation);

}  // namespace sonora::update
