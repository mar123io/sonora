// sonora-updater: the process that is allowed to move the installation.
//
//   sonora-updater --check --root <dir> [--name Sonora]
//       Asks the manifest whether there is a newer version and, if there is, leaves a
//       verified tree in <dir>/Sonora.new. Moves nothing. Safe to run from inside the
//       installation, and safe to run while the application is up.
//
//   sonora-updater --apply --root <dir> [--name Sonora] [--wait <pid>] [--relaunch]
//       Performs whatever the journal says is in flight: the swap, or the rollback. If
//       it is running from inside the installation it copies itself to the temporary
//       directory and re-executes there first, because Windows will not let a process
//       delete the tree it was loaded from.
//
// Two flags, and between them both directions of travel. See ADR 0011.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "sonora/core/version.h"
#include "sonora/platform/http.h"
#include "sonora/platform/update_host.h"
#include "sonora/update/updater.h"

namespace {

namespace fs = std::filesystem;
using sonora::update::Layout;

// Where the manifest lives, from the build rather than from an argument.
//
// A url the updater could be told at runtime is a url an attacker who can write one
// registry value can change, and then the signature is the only thing left standing --
// which it would be, but "the only thing left standing" is not where a design should
// start. There is no flag for this, and the end-to-end tests do not need one because
// they drive the library directly with their own Fetcher.
#ifndef SONORA_UPDATE_MANIFEST_URL
#define SONORA_UPDATE_MANIFEST_URL \
  "https://mar123io.github.io/sonora/updates/stable/manifest.json"
#endif
#ifndef SONORA_UPDATE_PLATFORM
#define SONORA_UPDATE_PLATFORM "win-x64"
#endif

struct Options {
  bool check = false;
  bool apply = false;
  bool relaunch = false;
  bool detached = false;  // set on the copy that runs from the temporary directory
  fs::path root;
  std::string name = "Sonora";
  std::uint32_t wait_for = 0;
};

int Usage() {
  std::fprintf(stderr,
               "usage: sonora-updater --check --root <dir>\n"
               "       sonora-updater --apply --root <dir> [--wait <pid>] [--relaunch]\n");
  return 2;
}

std::optional<Options> Parse(const std::vector<std::string>& args) {
  Options options;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& argument = args[i];
    const auto value = [&](std::string& out) {
      if (i + 1 >= args.size()) {
        return false;
      }
      out = args[++i];
      return true;
    };
    if (argument == "--check") {
      options.check = true;
    } else if (argument == "--apply") {
      options.apply = true;
    } else if (argument == "--relaunch") {
      options.relaunch = true;
    } else if (argument == "--detached") {
      options.detached = true;
    } else if (argument == "--root") {
      std::string text;
      if (!value(text)) {
        return std::nullopt;
      }
      options.root = fs::path(text);
    } else if (argument == "--name") {
      if (!value(options.name)) {
        return std::nullopt;
      }
    } else if (argument == "--wait") {
      std::string text;
      if (!value(text)) {
        return std::nullopt;
      }
      options.wait_for = static_cast<std::uint32_t>(std::strtoul(text.c_str(), nullptr, 10));
    } else {
      return std::nullopt;
    }
  }
  if (options.check == options.apply || options.root.empty() || options.name.empty()) {
    return std::nullopt;
  }
  return options;
}

bool RunningInside(const fs::path& tree) {
  const fs::path self = sonora::platform::ExecutablePath();
  if (self.empty()) {
    return true;  // if it cannot be known, assume the answer that is safe
  }
  std::error_code ec;
  const fs::path here = fs::weakly_canonical(self.parent_path(), ec);
  const fs::path there = fs::weakly_canonical(tree, ec);
  const auto mismatch = std::mismatch(there.begin(), there.end(), here.begin(), here.end());
  return mismatch.first == there.end();
}

int Check(const Options& options) {
  Layout layout;
  layout.root = options.root;
  layout.name = options.name;

  sonora::update::UpdateConfig config;
  config.manifest_url = SONORA_UPDATE_MANIFEST_URL;
  config.signature_url = std::string(SONORA_UPDATE_MANIFEST_URL) + ".sig";
  config.platform = SONORA_UPDATE_PLATFORM;

  const auto current = sonora::update::ParseVersion(sonora::core::kVersion);
  if (!current.has_value()) {
    std::fprintf(stderr, "sonora-updater: this build's version (%s) is not a release version\n",
                 sonora::core::kVersion);
    return 1;
  }

  // The one part of this program that touches a network, and it comes from the platform
  // layer: see src/platform/iface/include/sonora/platform/http.h. A platform that has
  // no updater yet returns nothing here rather than an implementation that cannot work.
  const std::unique_ptr<sonora::update::Fetcher> fetcher = sonora::platform::MakeHttpFetcher();
  if (fetcher == nullptr) {
    std::fprintf(stderr, "sonora-updater: this platform has no update client\n");
    return 1;
  }
  const sonora::update::StageOutcome outcome = sonora::update::CheckAndStage(
      layout, *fetcher, config, *current, sonora::platform::DurableFlush());

  std::fprintf(stderr, "sonora-updater: %.*s\n",
               static_cast<int>(Describe(outcome.result).size()),
               Describe(outcome.result).data());
  if (outcome.result == sonora::update::StageResult::kStaged) {
    std::fprintf(stderr, "sonora-updater: %s staged, %s, %llu bytes downloaded\n",
                 sonora::update::ToString(outcome.version).c_str(),
                 outcome.used_delta ? "as a delta" : "as a full package",
                 static_cast<unsigned long long>(outcome.downloaded));
  }
  if (!outcome.detail.empty()) {
    std::fprintf(stderr, "sonora-updater: %s\n", outcome.detail.c_str());
  }
  return outcome.result == sonora::update::StageResult::kStaged ||
                 outcome.result == sonora::update::StageResult::kUpToDate
             ? 0
             : 1;
}

int Apply(const Options& options) {
  Layout layout;
  layout.root = options.root;
  layout.name = options.name;

  if (!options.detached && RunningInside(layout.current())) {
    // Step out of the tree before touching it. The copy gets the same arguments plus
    // --detached, so it does not do this again -- and if the copy somehow ends up inside
    // the installation, --detached is what stops the recursion rather than a counter.
    const auto copy = sonora::platform::CopyExecutableToTemporary("sonora-updater");
    if (!copy.has_value()) {
      std::fprintf(stderr, "sonora-updater: cannot copy myself out of the installation\n");
      return 1;
    }
    std::vector<std::string> arguments = {
        "--apply", "--detached", "--root", layout.root.string(), "--name", layout.name};
    if (options.relaunch) {
      arguments.push_back("--relaunch");
    }
    // The pid to wait for is this process's parent in spirit: whoever asked. If nobody
    // said, wait for the process that started us.
    arguments.push_back("--wait");
    arguments.push_back(std::to_string(
        options.wait_for != 0 ? options.wait_for : sonora::platform::CurrentProcessId()));
    if (!sonora::platform::SpawnDetached(*copy, arguments)) {
      std::fprintf(stderr, "sonora-updater: cannot start the copy\n");
      return 1;
    }
    return 0;
  }

  if (options.wait_for != 0) {
    // Thirty seconds, and then go ahead anyway. A rename of a directory does not need
    // the application to be gone -- Windows allows renaming a directory whose child is a
    // running executable -- so waiting is politeness rather than a requirement, and
    // waiting forever for a process that is hung would mean never updating.
    static_cast<void>(sonora::platform::WaitForProcess(options.wait_for, 30000));
  }

  const sonora::update::StartupOutcome outcome =
      sonora::update::RecoverAtStartup(layout, sonora::platform::DurableFlush(), true);
  if (outcome.journal_unreadable) {
    std::fprintf(stderr, "sonora-updater: the journal cannot be read; nothing was changed\n");
    return 1;
  }
  std::fprintf(stderr, "sonora-updater: %.*s (stage now %.*s)\n",
               static_cast<int>(Describe(outcome.result).size()),
               Describe(outcome.result).data(),
               static_cast<int>(Describe(outcome.journal.stage).size()),
               Describe(outcome.journal.stage).data());

  if (options.relaunch) {
    const fs::path executable = layout.current() / (layout.name + ".exe");
    if (!sonora::platform::SpawnDetached(executable, {})) {
      std::fprintf(stderr, "sonora-updater: could not start %s\n", executable.string().c_str());
      return 1;
    }
  }
  return outcome.result == sonora::update::DriveResult::kDone ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  const std::vector<std::string> args(argv + 1, argv + argc);
  const auto options = Parse(args);
  if (!options.has_value()) {
    return Usage();
  }
  return options->check ? Check(*options) : Apply(*options);
}
