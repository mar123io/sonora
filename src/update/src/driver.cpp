#include "sonora/update/driver.h"

namespace sonora::update {

Runner::~Runner() = default;

std::string_view Describe(DriveResult result) {
  switch (result) {
    case DriveResult::kDone:
      return "done";
    case DriveResult::kNeedsOutOfProcess:
      return "the next step has to run outside the installation";
    case DriveResult::kJournalWriteFailed:
      return "the journal could not be written; nothing was changed";
    case DriveResult::kStepFailed:
      return "a step failed; the journal records what to retry";
    case DriveResult::kStalled:
      return "a state this version cannot resolve";
  }
  return "unknown";
}

DriveResult Drive(Journal& journal, Runner& runner, bool may_touch_installation) {
  for (int iteration = 0; iteration < kMaxDriverIterations; ++iteration) {
    const Facts facts = runner.Look();
    const Decision decision = NextStep(journal, facts);

    if (!may_touch_installation && TouchesInstallation(decision.step)) {
      // Nothing has been written, so the state on disk is exactly what the
      // out-of-tree copy will find when it starts.
      return DriveResult::kNeedsOutOfProcess;
    }

    const bool journal_changed = !(decision.journal == journal);
    if (journal_changed) {
      if (!runner.WriteJournal(decision.journal)) {
        return DriveResult::kJournalWriteFailed;
      }
      // Only after the write succeeded: if it did not, the caller's journal still
      // describes what is actually on the disk.
      journal = decision.journal;
    }

    if (decision.done()) {
      return DriveResult::kDone;
    }

    if (!runner.Perform(decision.step)) {
      return DriveResult::kStepFailed;
    }

    // No progress check here beyond the iteration bound, and deliberately so. A
    // step that succeeded without changing the facts -- a rename that reported
    // success and did nothing -- would loop, and the bound is what turns that into
    // a stop rather than a hang. Anything cleverer would be guessing about a
    // filesystem that is already lying.
    if (!journal_changed && decision.step == Step::kNothing) {
      return DriveResult::kDone;
    }
  }
  return DriveResult::kStalled;
}

}  // namespace sonora::update
