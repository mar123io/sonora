#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "sonora/update/driver.h"
#include "sonora/update/journal.h"
#include "sonora/update/version.h"

namespace {

using sonora::update::Decision;
using sonora::update::Drive;
using sonora::update::DriveResult;
using sonora::update::Facts;
using sonora::update::Journal;
using sonora::update::NextStep;
using sonora::update::Runner;
using sonora::update::Stage;
using sonora::update::Step;
using sonora::update::Version;

constexpr Version kOld{1, 0, 0};
constexpr Version kNew{1, 0, 1};

// The filesystem, as three directory slots and a flag. Every one of them is a
// value the real updater reads with one Win32 call, and nothing else about a disk
// is relevant to the decision -- which is the property ADR 0011 was after and the
// reason this test exists at all.
struct FakeFs {
  std::optional<Version> current;
  std::optional<Version> staged;
  std::optional<Version> previous;
  std::optional<Version> launch_ok;
  std::optional<Journal> journal;
};

// A budget of operations, shared between the runner and the caller, so that a test
// can say "stop the machine after the seventh thing that reached the disk".
struct Budget {
  int remaining = std::numeric_limits<int>::max();
  int spent = 0;
  bool exhausted = false;

  bool Spend() {
    if (remaining <= 0) {
      exhausted = true;
      return false;
    }
    --remaining;
    ++spent;
    return true;
  }
};

class FakeRunner final : public Runner {
 public:
  FakeRunner(FakeFs& fs, Budget& budget) : fs_(fs), budget_(budget) {}

  bool WriteJournal(const Journal& journal) override {
    if (!budget_.Spend()) {
      return false;
    }
    fs_.journal = journal;
    return true;
  }

  Facts Look() override {
    Facts facts;
    facts.current = fs_.current.has_value();
    facts.staged = fs_.staged.has_value();
    facts.previous = fs_.previous.has_value();
    facts.launch_ok = fs_.launch_ok;
    return facts;
  }

  bool Perform(Step step) override {
    // Before the budget, because asking for an impossible move is a bug in
    // NextStep whether or not the machine survives long enough to attempt it.
    if (!Possible(step)) {
      impossible_ = step;
      return false;
    }
    if (!budget_.Spend()) {
      return false;
    }
    switch (step) {
      case Step::kMoveCurrentToPrevious:
        fs_.previous = fs_.current;
        fs_.current.reset();
        break;
      case Step::kMoveStagedToCurrent:
        fs_.current = fs_.staged;
        fs_.staged.reset();
        break;
      case Step::kMovePreviousToCurrent:
        fs_.current = fs_.previous;
        fs_.previous.reset();
        break;
      case Step::kDiscardCurrent:
        fs_.current.reset();
        break;
      case Step::kRemovePrevious:
        fs_.previous.reset();
        break;
      case Step::kRemoveStaged:
        fs_.staged.reset();
        break;
      case Step::kNothing:
        break;
    }
    return true;
  }

  [[nodiscard]] std::optional<Step> impossible() const { return impossible_; }

 private:
  // What a rename or a delete can actually do. A move onto a name that is taken,
  // or of a directory that is not there, is an error from Windows and a mistake
  // here.
  [[nodiscard]] bool Possible(Step step) const {
    switch (step) {
      case Step::kNothing:
        return true;
      case Step::kMoveCurrentToPrevious:
        return fs_.current.has_value() && !fs_.previous.has_value();
      case Step::kMoveStagedToCurrent:
        return fs_.staged.has_value() && !fs_.current.has_value();
      case Step::kMovePreviousToCurrent:
        return fs_.previous.has_value() && !fs_.current.has_value();
      case Step::kDiscardCurrent:
        return fs_.current.has_value();
      case Step::kRemovePrevious:
        return fs_.previous.has_value();
      case Step::kRemoveStaged:
        return fs_.staged.has_value();
    }
    return false;
  }

  FakeFs& fs_;
  Budget& budget_;
  std::optional<Step> impossible_;
};

// One process start: read the journal off the disk, drive it as far as it goes.
DriveResult RunSession(FakeFs& fs, Budget& budget, std::optional<Step>& impossible) {
  Journal journal = fs.journal.value_or(Journal{});
  FakeRunner runner(fs, budget);
  const DriveResult result = Drive(journal, runner, /*may_touch_installation=*/true);
  if (runner.impossible().has_value()) {
    impossible = runner.impossible();
  }
  return result;
}

// The application runs. It writes the launch flag if the version installed is one
// that starts -- which is the whole of what the flag means.
void ApplicationRuns(FakeFs& fs, bool new_version_starts) {
  if (!fs.current.has_value()) {
    return;
  }
  const bool starts = (*fs.current == kOld) || new_version_starts;
  if (starts) {
    fs.launch_ok = fs.current;
  }
}

FakeFs StagedAndReady() {
  FakeFs fs;
  fs.current = kOld;
  fs.staged = kNew;
  fs.launch_ok = kOld;
  Journal journal;
  journal.stage = Stage::kStaged;
  journal.from = kOld;
  journal.to = kNew;
  fs.journal = journal;
  return fs;
}

struct Outcome {
  FakeFs fs;
  int operations = 0;
  bool stalled = false;
  std::optional<Step> impossible;
};

// The whole life of an update: the updater swaps at the exit of one run, and then
// the application starts a few times. `crash_after` stops the machine after that
// many operations have reached the disk; everything after that is recovery, with no
// budget, which is what a reboot is.
Outcome Live(bool new_version_starts, int crash_after) {
  Outcome outcome;
  outcome.fs = StagedAndReady();

  Budget limited;
  limited.remaining = crash_after;
  for (int session = 0; session < 4; ++session) {
    const DriveResult result = RunSession(outcome.fs, limited, outcome.impossible);
    if (result == DriveResult::kStalled) {
      outcome.stalled = true;
    }
    if (limited.exhausted) {
      break;  // the machine stopped; the application does not get to run
    }
    ApplicationRuns(outcome.fs, new_version_starts);
  }
  outcome.operations = limited.spent;

  Budget unlimited;
  for (int session = 0; session < 8; ++session) {
    const DriveResult result = RunSession(outcome.fs, unlimited, outcome.impossible);
    if (result == DriveResult::kStalled) {
      outcome.stalled = true;
    }
    ApplicationRuns(outcome.fs, new_version_starts);
  }
  return outcome;
}

void CheckSettled(const Outcome& outcome) {
  CHECK_FALSE(outcome.stalled);
  CHECK_FALSE(outcome.impossible.has_value());
  REQUIRE(outcome.fs.current.has_value());
  CHECK_FALSE(outcome.fs.staged.has_value());
  CHECK_FALSE(outcome.fs.previous.has_value());
  REQUIRE(outcome.fs.journal.has_value());
  CHECK(outcome.fs.journal->stage == Stage::kIdle);
}

}  // namespace

TEST_CASE("an update that starts ends up installed, and the old copy is gone") {
  const Outcome outcome = Live(/*new_version_starts=*/true, std::numeric_limits<int>::max());
  CheckSettled(outcome);
  CHECK(*outcome.fs.current == kNew);
  CHECK(outcome.fs.journal->refused.empty());
  CHECK(outcome.fs.launch_ok == kNew);
}

TEST_CASE("an update that will not start is rolled back and never offered again") {
  const Outcome outcome = Live(/*new_version_starts=*/false, std::numeric_limits<int>::max());
  CheckSettled(outcome);
  CHECK(*outcome.fs.current == kOld);
  REQUIRE(outcome.fs.journal->refused.size() == 1);
  CHECK(outcome.fs.journal->refused.front() == kNew);
}

TEST_CASE("the new version gets exactly one start before it is rolled back") {
  FakeFs fs = StagedAndReady();
  Budget budget;
  std::optional<Step> impossible;

  // The swap, at the exit of the old run.
  CHECK(RunSession(fs, budget, impossible) == DriveResult::kDone);
  REQUIRE(fs.current.has_value());
  CHECK(*fs.current == kNew);
  CHECK(fs.previous == kOld);
  CHECK(fs.journal->stage == Stage::kUnconfirmed);
  CHECK(fs.journal->attempts == 0);

  // The new version starts, and does not get far enough to write the flag.
  CHECK(RunSession(fs, budget, impossible) == DriveResult::kDone);
  CHECK(fs.journal->stage == Stage::kUnconfirmed);
  CHECK(fs.journal->attempts == 1);
  CHECK(*fs.current == kNew);  // still installed: it has had one chance, not none
  CHECK(fs.previous == kOld);

  // The start after that is the one that notices.
  CHECK(RunSession(fs, budget, impossible) == DriveResult::kDone);
  REQUIRE(fs.current.has_value());
  CHECK(*fs.current == kOld);
  CHECK_FALSE(fs.previous.has_value());
  CHECK(fs.journal->stage == Stage::kIdle);
  CHECK(fs.journal->refused == std::vector<Version>{kNew});
}

TEST_CASE("a version that starts slowly is not rolled back for being slow") {
  FakeFs fs = StagedAndReady();
  Budget budget;
  std::optional<Step> impossible;
  CHECK(RunSession(fs, budget, impossible) == DriveResult::kDone);

  // The updater looks before the application has had time to do anything. There is
  // no deadline here to miss: the flag is a milestone, so a slow machine simply
  // reaches it later in the same run.
  CHECK(RunSession(fs, budget, impossible) == DriveResult::kDone);
  CHECK(*fs.current == kNew);
  // ...and it reaches it.
  fs.launch_ok = kNew;
  CHECK(RunSession(fs, budget, impossible) == DriveResult::kDone);
  CHECK(*fs.current == kNew);
  CHECK_FALSE(fs.previous.has_value());
  CHECK(fs.journal->refused.empty());
}

TEST_CASE("stopping the machine at every point of an update that starts") {
  const int total = Live(true, std::numeric_limits<int>::max()).operations;
  REQUIRE(total > 0);
  for (int crash_after = 0; crash_after <= total; ++crash_after) {
    const Outcome outcome = Live(true, crash_after);
    CheckSettled(outcome);
    // Either the swap completed before the stop and the new version is in place,
    // or it did not and the old one is -- and never neither, and never both.
    const bool sane = *outcome.fs.current == kNew || *outcome.fs.current == kOld;
    CHECK(sane);
  }
}

TEST_CASE("stopping the machine at every point of an update that does not start") {
  const int total = Live(false, std::numeric_limits<int>::max()).operations;
  REQUIRE(total > 0);
  for (int crash_after = 0; crash_after <= total; ++crash_after) {
    const Outcome outcome = Live(false, crash_after);
    CheckSettled(outcome);
    // This is the one that matters. Whatever moment the power went, the machine
    // ends up on the version that works.
    CHECK(*outcome.fs.current == kOld);
  }
}

TEST_CASE("every state this machine can be in settles, from anywhere") {
  // Not a sample of interesting states: all of them. Seven stages, the eight
  // combinations of three directories, three things the launch flag can say, and
  // both attempt counts -- including the combinations that cannot be reached,
  // because "cannot be reached" is exactly the assumption that a half-finished
  // rollback on somebody's laptop turns out to break.
  int reachable_states = 0;
  for (const Stage stage : {Stage::kIdle, Stage::kStaging, Stage::kStaged, Stage::kMovingOut,
                            Stage::kMovingIn, Stage::kUnconfirmed, Stage::kRollingBack}) {
    for (int bits = 0; bits < 8; ++bits) {
      for (int flag = 0; flag < 3; ++flag) {
        for (int attempts = 0; attempts < 2; ++attempts) {
          for (const bool new_version_starts : {false, true}) {
            FakeFs fs;
            if ((bits & 1) != 0) {
              fs.current =
                  stage == Stage::kUnconfirmed || stage == Stage::kRollingBack ? kNew : kOld;
            }
            if ((bits & 2) != 0) {
              fs.staged = kNew;
            }
            if ((bits & 4) != 0) {
              fs.previous = kOld;
            }
            if (flag == 1) {
              fs.launch_ok = kOld;
            } else if (flag == 2) {
              fs.launch_ok = kNew;
            }
            Journal journal;
            journal.stage = stage;
            journal.from = kOld;
            journal.to = kNew;
            journal.attempts = attempts;
            fs.journal = journal;

            const bool installation_reachable =
                fs.current.has_value() || fs.previous.has_value() ||
                (stage == Stage::kStaged && fs.staged.has_value());

            Budget budget;
            std::optional<Step> impossible;
            bool stalled = false;
            for (int session = 0; session < 8; ++session) {
              if (RunSession(fs, budget, impossible) == DriveResult::kStalled) {
                stalled = true;
              }
              ApplicationRuns(fs, new_version_starts);
            }

            CHECK_FALSE(stalled);
            CHECK_FALSE(impossible.has_value());
            REQUIRE(fs.journal.has_value());
            CHECK(fs.journal->stage == Stage::kIdle);
            CHECK_FALSE(fs.staged.has_value());
            CHECK_FALSE(fs.previous.has_value());
            if (installation_reachable) {
              ++reachable_states;
              CHECK(fs.current.has_value());
            }
          }
        }
      }
    }
  }
  // A guard on the guard: if a future change made `installation_reachable` false
  // everywhere, every assertion above would pass and this test would be checking
  // nothing.
  CHECK(reachable_states > 300);
}

TEST_CASE("a staging tree is never installed, because nothing knows it is complete") {
  FakeFs fs;
  fs.current = kOld;
  fs.staged = kNew;
  fs.launch_ok = kOld;
  Journal journal;
  journal.stage = Stage::kStaging;
  journal.from = kOld;
  journal.to = kNew;
  fs.journal = journal;

  Budget budget;
  std::optional<Step> impossible;
  CHECK(RunSession(fs, budget, impossible) == DriveResult::kDone);
  CHECK(*fs.current == kOld);
  CHECK_FALSE(fs.staged.has_value());
  CHECK(fs.journal->stage == Stage::kIdle);
}

TEST_CASE("a journal from a version that did not write it stops everything") {
  FakeFs fs = StagedAndReady();
  Journal journal = *fs.journal;
  journal.schema = 99;
  fs.journal = journal;

  Budget budget;
  std::optional<Step> impossible;
  CHECK(RunSession(fs, budget, impossible) == DriveResult::kDone);
  // Untouched: no swap, no sweep, nothing. The installation is left exactly as it
  // was, which is the only safe answer when the record of what is in flight cannot
  // be read.
  CHECK(*fs.current == kOld);
  CHECK(fs.staged == kNew);
  CHECK(budget.spent == 0);
}

TEST_CASE("the application never moves the installation itself") {
  FakeFs fs = StagedAndReady();
  Journal journal = *fs.journal;
  Budget budget;
  FakeRunner runner(fs, budget);

  // This is what Sonora.exe does at startup: it reads the journal and finds that
  // the next step is a rename of the directory it was loaded from.
  CHECK(Drive(journal, runner, /*may_touch_installation=*/false) ==
        DriveResult::kNeedsOutOfProcess);
  // And it has changed nothing, so the out-of-tree copy finds the state the
  // decision was made from.
  CHECK(budget.spent == 0);
  CHECK(*fs.current == kOld);
  CHECK(fs.staged == kNew);
  CHECK(fs.journal->stage == Stage::kStaged);
}

TEST_CASE("a journal that cannot be written changes nothing") {
  FakeFs fs = StagedAndReady();
  Journal journal = *fs.journal;
  Budget budget;
  budget.remaining = 0;
  FakeRunner runner(fs, budget);

  CHECK(Drive(journal, runner, true) == DriveResult::kJournalWriteFailed);
  CHECK(*fs.current == kOld);
  CHECK(fs.staged == kNew);
  // The caller's journal still describes what is on the disk, not what was wanted.
  CHECK(journal.stage == Stage::kStaged);
}

TEST_CASE("the journal round-trips through the file it is written to") {
  Journal journal;
  journal.stage = Stage::kUnconfirmed;
  journal.from = Version{1, 2, 3};
  journal.to = Version{1, 2, 4};
  journal.attempts = 1;
  journal.refused = {Version{1, 1, 0}, Version{1, 1, 5}};

  sonora::update::JournalError error = sonora::update::JournalError::kNone;
  const auto again =
      sonora::update::ParseJournal(sonora::update::EncodeJournal(journal), error);
  REQUIRE(error == sonora::update::JournalError::kNone);
  REQUIRE(again.has_value());
  CHECK(*again == journal);
}

TEST_CASE("every stage round-trips through its name") {
  for (const Stage stage : {Stage::kIdle, Stage::kStaging, Stage::kStaged, Stage::kMovingOut,
                            Stage::kMovingIn, Stage::kUnconfirmed, Stage::kRollingBack}) {
    Journal journal;
    journal.stage = stage;
    sonora::update::JournalError error = sonora::update::JournalError::kNone;
    const auto again =
        sonora::update::ParseJournal(sonora::update::EncodeJournal(journal), error);
    REQUIRE(again.has_value());
    CHECK(again->stage == stage);
  }
}

TEST_CASE("a journal this version cannot read is an error, not an empty journal") {
  using sonora::update::JournalError;
  using sonora::update::ParseJournal;
  JournalError error = JournalError::kNone;

  CHECK_FALSE(ParseJournal("", error).has_value());
  CHECK(error == JournalError::kNotJson);
  CHECK_FALSE(ParseJournal("{", error).has_value());
  CHECK(error == JournalError::kNotJson);
  CHECK_FALSE(ParseJournal(R"({"schema": 2, "stage": "idle"})", error).has_value());
  CHECK(error == JournalError::kUnsupportedSchema);
  CHECK_FALSE(ParseJournal(R"({"stage": "idle"})", error).has_value());
  CHECK(error == JournalError::kUnsupportedSchema);
  CHECK_FALSE(ParseJournal(R"({"schema": 1, "stage": "halfway", "from": "1.0.0", "to": "1.0.1",
                       "attempts": 0})",
                           error)
                  .has_value());
  CHECK(error == JournalError::kBadField);
  CHECK_FALSE(ParseJournal(R"({"schema": 1, "stage": "idle", "from": "1.0",
                               "to": "1.0.1", "attempts": 0})",
                           error)
                  .has_value());
  CHECK(error == JournalError::kBadField);
  CHECK_FALSE(ParseJournal(R"({"schema": 1, "stage": "idle", "from": "1.0.0",
                               "to": "1.0.1", "attempts": -1})",
                           error)
                  .has_value());
  CHECK(error == JournalError::kBadField);
}

TEST_CASE("the refused list is bounded, and the oldest refusal is the one that goes") {
  Journal journal;
  journal.stage = Stage::kUnconfirmed;
  journal.attempts = sonora::update::kMaxLaunchAttempts;
  journal.from = kOld;
  for (std::size_t i = 0; i < sonora::update::kMaxRefusedVersions; ++i) {
    journal.refused.push_back(Version{0, 1, static_cast<std::uint32_t>(i)});
  }
  journal.to = kNew;

  Facts facts;
  facts.current = true;
  facts.previous = true;
  const Decision decision = NextStep(journal, facts);
  CHECK(decision.step == Step::kDiscardCurrent);
  CHECK(decision.journal.refused.size() == sonora::update::kMaxRefusedVersions);
  CHECK(decision.journal.refused.front() == Version{0, 1, 1});
  CHECK(decision.journal.refused.back() == kNew);
}

TEST_CASE("refusing the same version twice does not make the list grow") {
  Journal journal;
  journal.stage = Stage::kUnconfirmed;
  journal.attempts = sonora::update::kMaxLaunchAttempts;
  journal.from = kOld;
  journal.to = kNew;
  journal.refused = {kNew};

  Facts facts;
  facts.current = true;
  facts.previous = true;
  const Decision decision = NextStep(journal, facts);
  CHECK(decision.journal.refused == std::vector<Version>{kNew});
}
