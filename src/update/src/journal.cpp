#include "sonora/update/journal.h"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace sonora::update {
namespace {

using Json = nlohmann::json;

void Refuse(Journal& journal, const Version& version) {
  const bool present = std::any_of(journal.refused.begin(), journal.refused.end(),
                                   [&version](const Version& v) { return v == version; });
  if (present) {
    return;
  }
  // Bounded, and the oldest goes first. A list that grows without a limit is a
  // journal that eventually cannot be written, and the journal is the one file
  // this subsystem cannot afford to fail to write.
  if (journal.refused.size() >= kMaxRefusedVersions) {
    journal.refused.erase(journal.refused.begin());
  }
  journal.refused.push_back(version);
}

Journal GoIdle(const Journal& journal) {
  Journal next;
  next.schema = kJournalSchema;
  next.stage = Stage::kIdle;
  next.refused = journal.refused;
  return next;
}

// A tidy-up available from any terminal state: a staged tree with nothing in
// flight, or a previous tree nobody is going back to, is garbage taking 213 MiB.
std::optional<Step> Sweep(const Facts& facts) {
  if (facts.staged) {
    return Step::kRemoveStaged;
  }
  if (facts.previous) {
    return Step::kRemovePrevious;
  }
  return std::nullopt;
}

}  // namespace

std::string_view Describe(Stage stage) {
  switch (stage) {
    case Stage::kIdle:
      return "idle";
    case Stage::kStaging:
      return "staging";
    case Stage::kStaged:
      return "staged";
    case Stage::kMovingOut:
      return "moving-out";
    case Stage::kMovingIn:
      return "moving-in";
    case Stage::kUnconfirmed:
      return "unconfirmed";
    case Stage::kRollingBack:
      return "rolling-back";
  }
  return "unknown";
}

std::string_view Describe(Step step) {
  switch (step) {
    case Step::kNothing:
      return "nothing";
    case Step::kMoveCurrentToPrevious:
      return "move the installation aside";
    case Step::kMoveStagedToCurrent:
      return "move the staged version into place";
    case Step::kMovePreviousToCurrent:
      return "move the previous version back into place";
    case Step::kDiscardCurrent:
      return "remove the version that would not start";
    case Step::kRemovePrevious:
      return "remove the previous version";
    case Step::kRemoveStaged:
      return "remove the staged version";
  }
  return "unknown";
}

bool TouchesInstallation(Step step) {
  switch (step) {
    case Step::kNothing:
    case Step::kRemoveStaged:
      return false;
    case Step::kMoveCurrentToPrevious:
    case Step::kMoveStagedToCurrent:
    case Step::kMovePreviousToCurrent:
    case Step::kDiscardCurrent:
    case Step::kRemovePrevious:
      return true;
  }
  return true;
}

Decision NextStep(const Journal& journal, const Facts& facts) {
  Decision decision;
  decision.journal = journal;

  // A journal from a version that did not write it. Nothing at all: leaving an
  // installation alone is the only safe answer when the record of what is in
  // flight cannot be read.
  if (journal.schema != kJournalSchema) {
    return decision;
  }

  switch (journal.stage) {
    case Stage::kIdle: {
      // An idle journal with no installation but a previous tree next to it. The
      // exhaustive test found this one, and the version of this function before it
      // swept the previous tree away -- deleting the only complete installation on
      // the machine because the journal said there was nothing in flight.
      //
      // Reachable, too, and not exotically: a rollback that finished its rename and
      // then lost power before its journal write is indistinguishable from this,
      // because the journal it would have written is the one already there.
      if (!facts.current && facts.previous) {
        decision.journal.stage = Stage::kRollingBack;
        decision.step = Step::kMovePreviousToCurrent;
        return decision;
      }
      if (const auto sweep = Sweep(facts)) {
        decision.step = *sweep;
      }
      return decision;
    }

    case Stage::kStaging: {
      // The staged tree was being written when something stopped. Its contents
      // are unknown and therefore worthless: nothing in this design can tell a
      // complete tree from one that is missing its last file, which is exactly
      // why "being written" is a stage of its own.
      if (facts.staged) {
        decision.step = Step::kRemoveStaged;
        return decision;
      }
      decision.journal = GoIdle(journal);
      return decision;
    }

    case Stage::kStaged: {
      if (!facts.staged) {
        // Somebody cleaned it up, or the disk did. Nothing lost but a download.
        decision.journal = GoIdle(journal);
        return decision;
      }
      if (facts.previous) {
        // A leftover from an update that was confirmed but not swept. It has to go
        // before the swap, because the swap is about to need that name.
        decision.step = Step::kRemovePrevious;
        return decision;
      }
      if (!facts.current) {
        // No installation to move aside: skip straight to putting the staged tree
        // in its place. This is reachable from a rollback that lost both trees.
        decision.journal.stage = Stage::kMovingIn;
        decision.step = Step::kMoveStagedToCurrent;
        return decision;
      }
      decision.journal.stage = Stage::kMovingOut;
      decision.step = Step::kMoveCurrentToPrevious;
      return decision;
    }

    case Stage::kMovingOut: {
      if (facts.current && facts.previous) {
        // Both names taken: the rename could not have happened, and something
        // else left a previous tree behind. Clear it and try again.
        decision.step = Step::kRemovePrevious;
        return decision;
      }
      if (facts.current) {
        // The rename has not happened yet. Repeating it is the whole point of
        // recording the intent first.
        decision.step = Step::kMoveCurrentToPrevious;
        return decision;
      }
      // current is gone. Either the rename succeeded (previous exists) or both
      // trees are missing, and in both cases the way forward is the staged tree.
      if (facts.staged) {
        decision.journal.stage = Stage::kMovingIn;
        decision.step = Step::kMoveStagedToCurrent;
        return decision;
      }
      if (facts.previous) {
        // No staged tree left to install: put back what was moved aside.
        decision.journal.stage = Stage::kRollingBack;
        decision.step = Step::kMovePreviousToCurrent;
        return decision;
      }
      // Nothing anywhere. This is the one unrecoverable state, and it is only
      // reachable if something outside this state machine deleted a tree.
      decision.journal = GoIdle(journal);
      return decision;
    }

    case Stage::kMovingIn: {
      if (facts.current) {
        // In place. From here the question is no longer about directories, it is
        // whether the thing in them starts.
        decision.journal.stage = Stage::kUnconfirmed;
        decision.journal.attempts = 0;
        return decision;
      }
      if (facts.staged) {
        decision.step = Step::kMoveStagedToCurrent;
        return decision;
      }
      if (facts.previous) {
        decision.journal.stage = Stage::kRollingBack;
        decision.step = Step::kMovePreviousToCurrent;
        return decision;
      }
      decision.journal = GoIdle(journal);
      return decision;
    }

    case Stage::kUnconfirmed: {
      if (!facts.current) {
        // The installed tree vanished between the swap and this look.
        if (facts.previous) {
          decision.journal.stage = Stage::kRollingBack;
          decision.step = Step::kMovePreviousToCurrent;
          return decision;
        }
        if (facts.staged) {
          // Not a state this machine can reach on its own, and the answer is still
          // the same one as everywhere else: an installation that exists beats one
          // that does not.
          decision.journal.stage = Stage::kMovingIn;
          decision.step = Step::kMoveStagedToCurrent;
          return decision;
        }
        decision.journal = GoIdle(journal);
        return decision;
      }
      if (facts.launch_ok.has_value() && *facts.launch_ok == journal.to) {
        // It started. The previous tree is no longer insurance, it is 213 MiB.
        if (const auto sweep = Sweep(facts)) {
          decision.step = *sweep;
          return decision;
        }
        decision.journal = GoIdle(journal);
        return decision;
      }
      if (journal.attempts < kMaxLaunchAttempts) {
        // Record that a chance is being given, and then give it. Without this
        // write, a version that crashes before the flag would be retried forever,
        // because nothing would remember that it had already had its turn.
        decision.journal.attempts = journal.attempts + 1;
        return decision;
      }
      if (!facts.previous) {
        // Nothing to go back to. The installation stays as it is and stops
        // pretending an update is in flight; recovery here is a reinstall.
        decision.journal = GoIdle(journal);
        return decision;
      }
      decision.journal.stage = Stage::kRollingBack;
      Refuse(decision.journal, journal.to);
      decision.step = Step::kDiscardCurrent;
      return decision;
    }

    case Stage::kRollingBack: {
      if (facts.current && facts.previous) {
        decision.step = Step::kDiscardCurrent;
        return decision;
      }
      if (!facts.current && facts.previous) {
        decision.step = Step::kMovePreviousToCurrent;
        return decision;
      }
      if (facts.current) {
        // Back where it started.
        if (facts.staged) {
          decision.step = Step::kRemoveStaged;
          return decision;
        }
        decision.journal = GoIdle(journal);
        return decision;
      }
      // No current and no previous: fall forward if there is anything to fall
      // forward to, because an installation that exists beats one that does not.
      if (facts.staged) {
        decision.journal.stage = Stage::kMovingIn;
        decision.step = Step::kMoveStagedToCurrent;
        return decision;
      }
      decision.journal = GoIdle(journal);
      return decision;
    }
  }

  return decision;
}

std::string EncodeJournal(const Journal& journal) {
  Json root;
  root["schema"] = kJournalSchema;
  root["stage"] = std::string(Describe(journal.stage));
  root["from"] = ToString(journal.from);
  root["to"] = ToString(journal.to);
  root["attempts"] = journal.attempts;
  Json refused = Json::array();
  for (const Version& version : journal.refused) {
    refused.push_back(ToString(version));
  }
  root["refused"] = std::move(refused);
  // Two-space indent: this file is read by a person exactly once, on the day an
  // installation will not start, and that is not the day to be parsing one line.
  return root.dump(2);
}

std::string_view Describe(JournalError error) {
  switch (error) {
    case JournalError::kNone:
      return "no error";
    case JournalError::kNotJson:
      return "the journal is not valid JSON";
    case JournalError::kUnsupportedSchema:
      return "a journal schema this version does not understand";
    case JournalError::kBadField:
      return "a field the journal cannot hold";
    case JournalError::kTooMany:
      return "more refused versions than the journal holds";
  }
  return "unknown error";
}

std::optional<Journal> ParseJournal(std::string_view json, JournalError& error) {
  error = JournalError::kNone;

  if (json.empty() || json.size() > 64 * 1024) {
    error = JournalError::kNotJson;
    return std::nullopt;
  }
  const Json root = Json::parse(json, nullptr, false);
  if (root.is_discarded() || !root.is_object()) {
    error = JournalError::kNotJson;
    return std::nullopt;
  }

  Journal journal;
  if (!root.contains("schema") || !root["schema"].is_number_integer()) {
    error = JournalError::kUnsupportedSchema;
    return std::nullopt;
  }
  journal.schema = root["schema"].get<int>();
  if (journal.schema != kJournalSchema) {
    error = JournalError::kUnsupportedSchema;
    return std::nullopt;
  }

  if (!root.contains("stage") || !root["stage"].is_string()) {
    error = JournalError::kBadField;
    return std::nullopt;
  }
  const std::string& stage = root["stage"].get_ref<const std::string&>();
  bool found = false;
  for (const Stage candidate :
       {Stage::kIdle, Stage::kStaging, Stage::kStaged, Stage::kMovingOut, Stage::kMovingIn,
        Stage::kUnconfirmed, Stage::kRollingBack}) {
    if (Describe(candidate) == stage) {
      journal.stage = candidate;
      found = true;
      break;
    }
  }
  if (!found) {
    error = JournalError::kBadField;
    return std::nullopt;
  }

  const auto read_version = [&root, &error](const char* key, Version& out) {
    if (!root.contains(key) || !root[key].is_string()) {
      error = JournalError::kBadField;
      return false;
    }
    const auto parsed = ParseVersion(root[key].get_ref<const std::string&>());
    if (!parsed.has_value()) {
      error = JournalError::kBadField;
      return false;
    }
    out = *parsed;
    return true;
  };
  if (!read_version("from", journal.from) || !read_version("to", journal.to)) {
    return std::nullopt;
  }

  if (!root.contains("attempts") || !root["attempts"].is_number_integer()) {
    error = JournalError::kBadField;
    return std::nullopt;
  }
  journal.attempts = root["attempts"].get<int>();
  if (journal.attempts < 0 || journal.attempts > 1000) {
    error = JournalError::kBadField;
    return std::nullopt;
  }

  if (root.contains("refused")) {
    if (!root["refused"].is_array()) {
      error = JournalError::kBadField;
      return std::nullopt;
    }
    if (root["refused"].size() > kMaxRefusedVersions) {
      error = JournalError::kTooMany;
      return std::nullopt;
    }
    for (const Json& node : root["refused"]) {
      if (!node.is_string()) {
        error = JournalError::kBadField;
        return std::nullopt;
      }
      const auto parsed = ParseVersion(node.get_ref<const std::string&>());
      if (!parsed.has_value()) {
        error = JournalError::kBadField;
        return std::nullopt;
      }
      journal.refused.push_back(*parsed);
    }
  }

  return journal;
}

}  // namespace sonora::update
