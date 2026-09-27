#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "sonora/update/version.h"

namespace sonora::update {

// The updater's state machine, and the only part of this subsystem that can
// destroy an installation. ADR 0011 is the argument; this is the shape.
//
// Four directories, all siblings, so that every move is a rename within one
// directory rather than a copy across volumes:
//
//   <root>/Sonora           the installation
//   <root>/Sonora.new       the staged tree
//   <root>/Sonora.old       the previous tree, kept until the new one has started
//   <root>/Sonora.update/   journal and launch flag -- outside every tree, because
//                           the installation has to contain exactly the archive's
//                           members and nothing else, or the client can never
//                           reconstruct its own archive to patch from.
//
// NextStep is a pure function of the journal and four facts about those
// directories. It returns one step at a time and the journal to write before
// performing it. It never does anything, never reads a file, and is therefore
// exercisable at every point at which a machine could stop -- which is what the
// crash-injection tests do, for every such point, including the ones inside a
// rollback that is itself recovering from a failed update.

inline constexpr int kJournalSchema = 1;

// The number of attempts a new version gets to write the launch flag before it is
// rolled back. One: see ADR 0011 on why the cheap mistake is the early one.
inline constexpr int kMaxLaunchAttempts = 1;

// A bound on the driver loop. Every transition below either changes the stage or
// removes a directory, so the sequence is finite; the bound is here so that a
// journal from a future version, or a filesystem that keeps answering the same
// impossible thing, produces a stop rather than a spin.
inline constexpr int kMaxDriverIterations = 16;

enum class Stage {
  kIdle,         // nothing in flight
  kStaging,      // <root>/Sonora.new is being written and must not be trusted
  kStaged,       // it is complete and its hash matched
  kMovingOut,    // about to rename Sonora -> Sonora.old
  kMovingIn,     // about to rename Sonora.new -> Sonora
  kUnconfirmed,  // the new tree is installed; it has not yet proved it can start
  kRollingBack,  // it did not, and the previous tree is going back
};

[[nodiscard]] std::string_view Describe(Stage stage);

struct Journal {
  int schema = kJournalSchema;
  Stage stage = Stage::kIdle;
  Version from;  // what was installed before the swap
  Version to;    // what is being installed
  int attempts = 0;
  // Versions that were installed and could not start. Never retried; surfaced in
  // the about panel, because a refusal the user cannot see looks like an updater
  // that has stopped working.
  std::vector<Version> refused;

  [[nodiscard]] friend bool operator==(const Journal&, const Journal&) = default;
};

// What the filesystem says. Deliberately four values and not a filesystem: the
// decision cannot depend on anything it has not been handed.
struct Facts {
  bool current = false;   // <root>/Sonora
  bool staged = false;    // <root>/Sonora.new
  bool previous = false;  // <root>/Sonora.old
  // The version in <root>/Sonora.update/launch-ok, which the application writes
  // every time it gets far enough to show its window with the UI loaded.
  std::optional<Version> launch_ok;
};

enum class Step {
  kNothing,
  kMoveCurrentToPrevious,  // Sonora    -> Sonora.old
  kMoveStagedToCurrent,    // Sonora.new -> Sonora
  kMovePreviousToCurrent,  // Sonora.old -> Sonora
  kDiscardCurrent,         // remove Sonora (the version that would not start)
  kRemovePrevious,
  kRemoveStaged,
};

[[nodiscard]] std::string_view Describe(Step step);

// True for the steps that move or remove a directory that the running process may
// have been loaded from. Those must happen in the copy of the updater that runs
// outside the tree; Sonora.exe, seeing one of them, launches that copy and exits.
[[nodiscard]] bool TouchesInstallation(Step step);

struct Decision {
  // Write this journal (if it differs from the one passed in) and then perform
  // this step. Writing first is what makes a crash between the two recoverable:
  // an intent recorded but not carried out is retried, and the retry is a rename
  // that has already happened, which is a no-op.
  Journal journal;
  Step step = Step::kNothing;

  [[nodiscard]] bool done() const noexcept { return step == Step::kNothing; }
};

[[nodiscard]] Decision NextStep(const Journal& journal, const Facts& facts);

// The journal on disk, as JSON, written to a temporary file and renamed over the
// old one so that a crash during the write leaves the previous journal rather than
// half of a new one.
[[nodiscard]] std::string EncodeJournal(const Journal& journal);

enum class JournalError {
  kNone,
  kNotJson,
  kUnsupportedSchema,  // refused; the updater then does nothing at all
  kBadField,
  kTooMany,
};

[[nodiscard]] std::string_view Describe(JournalError error);

// Total. A journal that does not parse is not an empty journal: the caller is
// expected to leave the installation alone rather than assume idle, because
// "assume idle" on an unreadable journal means walking away from a swap that is
// half done.
[[nodiscard]] std::optional<Journal> ParseJournal(std::string_view json, JournalError& error);

inline constexpr std::size_t kMaxRefusedVersions = 32;

}  // namespace sonora::update
