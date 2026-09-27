#include "cef/update_service.h"

#include <cstdio>
#include <string>
#include <vector>

#include <sonora/core/version.h>
#include <sonora/platform/update_host.h>
#include <sonora/update/updater.h>
#include <sonora/update/version.h>

#include "cef/timer.h"

namespace sonora::shell {
namespace {

// Six hours, from the roadmap, and it is a number rather than a measurement: there is
// nothing to measure. What it trades is how long somebody runs a version with a fixed bug
// against how often every installation asks a question whose answer is almost always no.
constexpr std::int64_t kCheckIntervalMs = 6 * 60 * 60 * 1000;

// Not at the moment the window appears. The first minute of a start is when the library
// scan, the CEF cache warm-up and whatever the machine was already doing are competing,
// and an update check is the least urgent thing in the building.
constexpr std::int64_t kFirstCheckDelayMs = 90 * 1000;

CefRefPtr<ShellTimer>& CheckTimer() {
  static CefRefPtr<ShellTimer> timer;
  return timer;
}

bool& LaunchConfirmed() {
  static bool confirmed = false;
  return confirmed;
}

std::optional<std::filesystem::path> UpdaterExecutable() {
  const auto self = platform::ExecutablePath();
  if (!self.has_value()) {
    return std::nullopt;
  }
  std::filesystem::path updater = self->parent_path() / "sonora-updater";
  if (!self->extension().empty()) {
    updater += self->extension();  // ".exe", without this file having to know that
  }
  return updater;
}

std::vector<std::string> RootArguments() {
  const update::Layout layout = UpdateLayout();
  return {"--root", layout.root.string(), "--name", layout.name};
}

bool Spawn(const std::vector<std::string>& arguments) {
  const auto updater = UpdaterExecutable();
  if (!updater.has_value()) {
    return false;
  }
  return platform::SpawnDetached(*updater, arguments);
}

std::optional<update::Version> ThisVersion() {
  return update::ParseVersion(core::kVersion);
}

}  // namespace

update::Layout UpdateLayout() {
  update::Layout layout;
  const auto self = platform::ExecutablePath();
  if (!self.has_value()) {
    // No executable path means no updater. Pointing the layout at the current directory
    // would put a journal somewhere arbitrary, which is worse than a layout that finds
    // nothing.
    return layout;
  }
  const std::filesystem::path installation = self->parent_path();
  layout.root = installation.parent_path();
  layout.name = installation.filename().string();
  return layout;
}

StartupAction RecoverBeforeStartup() {
  const update::Layout layout = UpdateLayout();
  if (layout.root.empty()) {
    return StartupAction::kContinue;
  }

  const update::StartupOutcome outcome = update::RecoverAtStartup(
      layout, platform::DurableFlush(), /*may_touch_installation=*/false);
  if (outcome.journal_unreadable) {
    std::fprintf(stderr,
                 "update: the journal cannot be read; leaving the installation alone\n");
    return StartupAction::kContinue;
  }

  if (outcome.result == update::DriveResult::kNeedsOutOfProcess) {
    // A swap or a rollback is waiting, and this process is standing in the directory it
    // would have to move. Hand it over and leave: the updater waits for this process to
    // exit and then starts the version it put in place.
    std::vector<std::string> arguments = {"--apply"};
    for (const std::string& argument : RootArguments()) {
      arguments.push_back(argument);
    }
    arguments.push_back("--wait");
    arguments.push_back(std::to_string(platform::CurrentProcessId()));
    arguments.push_back("--relaunch");
    if (!Spawn(arguments)) {
      // Nothing was moved, so carrying on runs the version that is installed -- which is
      // either the new one waiting to be confirmed or the old one waiting to come back.
      // Both start. Refusing to start because an update could not be applied would turn
      // a missing file into a dead application.
      std::fprintf(stderr, "update: cannot start the updater; continuing as we are\n");
      return StartupAction::kContinue;
    }
    std::printf("update: handing over to the updater and exiting\n");
    return StartupAction::kHandOff;
  }

  if (outcome.journal.stage != update::Stage::kIdle) {
    std::printf("update: stage %s\n", std::string(Describe(outcome.journal.stage)).c_str());
  }
  return StartupAction::kContinue;
}

void StartPeriodicCheck() {
  const auto version = ThisVersion();
  if (!version.has_value()) {
    // A development build, whose version is not a release version. Checking would compare
    // 0.6.0 against a manifest and offer an update to the thing already being built.
    std::printf("update: not a release build, so no update checks\n");
    return;
  }

  const auto check = [] {
    std::vector<std::string> arguments = {"--check"};
    for (const std::string& argument : RootArguments()) {
      arguments.push_back(argument);
    }
    if (!Spawn(arguments)) {
      std::fprintf(stderr, "update: cannot start the update check\n");
    }
  };

  // One shortly after startup and then every six hours, and both are one-shot spawns of a
  // separate process: nothing about an update runs inside this one, so a check that hangs
  // on a captive-portal network cannot hold a thread here.
  static CefRefPtr<ShellTimer> first = ShellTimer::Once(kFirstCheckDelayMs, check);
  CheckTimer() = ShellTimer::Every(kCheckIntervalMs, check);
}

void NotifyUiLoaded() {
  if (LaunchConfirmed()) {
    return;
  }
  const auto version = ThisVersion();
  const update::Layout layout = UpdateLayout();
  if (!version.has_value() || layout.root.empty()) {
    return;
  }
  LaunchConfirmed() = true;
  if (!update::ConfirmLaunch(layout, *version, platform::DurableFlush())) {
    // Worth a line, because the consequence is a version that works being rolled back at
    // the next start. It is also not recoverable from here: if this write fails the disk
    // has a problem that an update is the least of.
    std::fprintf(stderr, "update: could not record that this version started\n");
    return;
  }
  std::printf("update: %s started and said so\n", core::kVersion);
}

void ApplyStagedUpdateAtExit() {
  const update::Layout layout = UpdateLayout();
  if (layout.root.empty()) {
    return;
  }
  const update::LoadedJournal loaded = update::LoadJournal(layout);
  if (!loaded.readable || loaded.journal.stage != update::Stage::kStaged) {
    return;
  }

  std::vector<std::string> arguments = {"--apply"};
  for (const std::string& argument : RootArguments()) {
    arguments.push_back(argument);
  }
  arguments.push_back("--wait");
  arguments.push_back(std::to_string(platform::CurrentProcessId()));
  // No --relaunch. An update applied because somebody closed Sonora should not reopen it:
  // the swap happens now and the new version appears the next time they ask for it.
  if (!Spawn(arguments)) {
    std::fprintf(stderr, "update: %s is staged but the updater could not be started\n",
                 update::ToString(loaded.journal.to).c_str());
    return;
  }
  std::printf("update: applying %s on the way out\n",
              update::ToString(loaded.journal.to).c_str());
}

std::string UpdateStatusSummary() {
  const update::Layout layout = UpdateLayout();
  if (layout.root.empty()) {
    return "no updater";
  }
  const update::LoadedJournal loaded = update::LoadJournal(layout);
  if (!loaded.readable) {
    return "the update journal cannot be read";
  }
  std::string summary(Describe(loaded.journal.stage));
  if (!loaded.journal.refused.empty()) {
    summary += "; refused: ";
    for (std::size_t i = 0; i < loaded.journal.refused.size(); ++i) {
      if (i > 0) {
        summary += ", ";
      }
      summary += update::ToString(loaded.journal.refused[i]);
    }
  }
  return summary;
}

}  // namespace sonora::shell
