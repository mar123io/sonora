#include "sonora/update/updater.h"

#include <algorithm>
#include <system_error>
#include <utility>

#include "sonora/update/patch.h"
#include "sonora/update/rollout.h"

namespace sonora::update {
namespace {

constexpr std::size_t kMaxSignatureBytes = 256;

// How much room an update needs, as a multiple of the payload: the reconstruction of
// the installed archive, the new archive the patch produces, and the tree unpacked
// out of it. Two and a half, rounded up to three, because a check that is only just
// satisfied fails on the machine that has a page file.
constexpr std::uint64_t kSpaceMultiple = 3;

std::span<const std::uint8_t> Bytes(const std::vector<std::uint8_t>& bytes) {
  return bytes;
}

std::string_view AsText(const std::vector<std::uint8_t>& bytes) {
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// The release entry for the version that is running, when the manifest still lists
// it. Its absence is normal -- a manifest keeps a handful of recent releases -- and
// it only costs the cheap check in step 4.
const Release* FindRelease(const Manifest& manifest,
                           std::string_view platform,
                           const Version& version) {
  for (const Release& release : manifest.releases) {
    if (release.platform == platform && release.version == version) {
      return &release;
    }
  }
  return nullptr;
}

}  // namespace

Fetcher::~Fetcher() = default;

std::string_view Describe(StageResult result) {
  switch (result) {
    case StageResult::kUpToDate:
      return "up to date";
    case StageResult::kStaged:
      return "an update is staged and waiting for the application to exit";
    case StageResult::kBusy:
      return "an update is already in flight";
    case StageResult::kJournalUnreadable:
      return "the update journal cannot be read, so nothing was touched";
    case StageResult::kManifestUnavailable:
      return "the manifest could not be fetched";
    case StageResult::kSignatureInvalid:
      return "the manifest is not signed by a key this version trusts";
    case StageResult::kManifestInvalid:
      return "the manifest is not one this version understands";
    case StageResult::kDownloadFailed:
      return "a download failed";
    case StageResult::kArtifactInvalid:
      return "a download is not what the manifest says it is";
    case StageResult::kNoRoom:
      return "not enough room on the disk to apply an update";
    case StageResult::kStagingFailed:
      return "the update could not be written to the disk";
  }
  return "unknown";
}

StageOutcome CheckAndStage(const Layout& layout,
                           Fetcher& fetcher,
                           const UpdateConfig& config,
                           const Version& current,
                           const FlushFn& flush) {
  StageOutcome outcome;

  const LoadedJournal loaded = LoadJournal(layout);
  if (!loaded.readable) {
    outcome.result = StageResult::kJournalUnreadable;
    return outcome;
  }
  if (loaded.journal.stage != Stage::kIdle) {
    outcome.result = StageResult::kBusy;
    outcome.detail = std::string(Describe(loaded.journal.stage));
    return outcome;
  }

  // 1 and 2: the manifest's bytes, and the signature over them, before any parse.
  const auto manifest_bytes = fetcher.Get(config.manifest_url, kMaxManifestBytes);
  const auto signature_bytes = fetcher.Get(config.signature_url, kMaxSignatureBytes);
  if (!manifest_bytes.has_value() || !signature_bytes.has_value()) {
    outcome.result = StageResult::kManifestUnavailable;
    return outcome;
  }
  outcome.downloaded += manifest_bytes->size() + signature_bytes->size();

  // The signature file is hex, with whatever trailing newline the tool that wrote it
  // felt like adding.
  std::string_view signature_text = AsText(*signature_bytes);
  while (!signature_text.empty() &&
         (signature_text.back() == '\n' || signature_text.back() == '\r' ||
          signature_text.back() == ' ')) {
    signature_text.remove_suffix(1);
  }
  const auto signature = ParseSignature(signature_text);
  const std::span<const PublicKey> keys = config.keys.empty() ? ReleaseKeys() : config.keys;
  if (!signature.has_value() || !VerifyDetached(Bytes(*manifest_bytes), *signature, keys)) {
    outcome.result = StageResult::kSignatureInvalid;
    return outcome;
  }

  // 3: and only now is a parser allowed to look at it.
  ManifestError manifest_error = ManifestError::kNone;
  const auto manifest = ParseManifest(AsText(*manifest_bytes), manifest_error);
  if (!manifest.has_value()) {
    outcome.result = StageResult::kManifestInvalid;
    outcome.detail = std::string(Describe(manifest_error));
    return outcome;
  }

  // The install id, created here on first use. Its absence declines partial rollouts
  // rather than joining them: see ADR 0012.
  const auto install = ReadOrCreateInstallId(layout, flush);

  const auto target =
      ChooseUpdate(*manifest, config.platform, current, loaded.journal.refused, install);
  if (!target.has_value()) {
    outcome.result = StageResult::kUpToDate;
    return outcome;
  }
  outcome.version = target->version;
  if (install.has_value()) {
    outcome.bucket = RolloutBucket(*install, target->version);
  }

  std::error_code ec;
  const fs::space_info space = fs::space(layout.root, ec);
  if (!ec && space.available < target->archive_size * kSpaceMultiple) {
    outcome.result = StageResult::kNoRoom;
    outcome.detail = "needs about " +
                     std::to_string(target->archive_size * kSpaceMultiple / (1024 * 1024)) +
                     " MiB";
    return outcome;
  }

  // From here on the journal says an update is being written, so a crash leaves a
  // tree nobody will trust rather than one somebody might install.
  Journal journal = loaded.journal;
  journal.stage = Stage::kStaging;
  journal.from = current;
  journal.to = target->version;
  journal.attempts = 0;
  FileSystemRunner runner(layout, flush);
  if (!runner.WriteJournal(journal)) {
    outcome.result = StageResult::kStagingFailed;
    outcome.detail = runner.last_error();
    return outcome;
  }

  const auto give_up = [&](StageResult result, std::string detail) {
    outcome.result = result;
    outcome.detail = std::move(detail);
    FileStoreError store_error = FileStoreError::kNone;
    static_cast<void>(RemoveTree(layout.staged(), store_error));
    static_cast<void>(RemoveTree(layout.download_dir(), store_error));
    Journal idle;
    idle.refused = journal.refused;
    static_cast<void>(runner.WriteJournal(idle));
    return outcome;
  };

  FileStoreError store_error = FileStoreError::kNone;
  if (!RemoveTree(layout.staged(), store_error) ||
      !RemoveTree(layout.download_dir(), store_error)) {
    return give_up(StageResult::kStagingFailed, std::string(Describe(store_error)));
  }
  fs::create_directories(layout.download_dir(), ec);

  const fs::path new_archive = layout.download_dir() / "new.spk";
  std::vector<std::uint8_t> archive_bytes;

  // 4: the installation, packed, and checked against the manifest when the manifest
  // still remembers the version that is running.
  bool try_delta = target->delta.has_value();
  const fs::path own_archive = layout.download_dir() / "current.spk";
  if (try_delta) {
    const auto packed = PackDirectory(layout.current(), own_archive, store_error);
    if (!packed.has_value()) {
      try_delta = false;
      outcome.detail =
          "could not pack the installed version: " + std::string(Describe(store_error));
    } else if (const Release* running = FindRelease(*manifest, config.platform, current)) {
      const auto own_hash = HashFile(own_archive, store_error);
      if (!own_hash.has_value() || !HashesEqual(*own_hash, running->archive.hash)) {
        // The installation is not byte-for-byte what the release published: a file
        // touched by something else, a half-finished earlier update, a build from a
        // different machine. Nothing is wrong and nothing can be patched.
        try_delta = false;
        outcome.detail = "the installed version does not match its published package";
      }
    }
  }

  if (try_delta) {
    const auto delta = fetcher.Get(target->delta->url, target->delta->size);
    if (!delta.has_value() || delta->size() != target->delta->size) {
      try_delta = false;
      outcome.detail = "the delta could not be fetched";
    } else {
      outcome.downloaded += delta->size();
      const auto delta_hash = HashBytes(*delta);
      if (!delta_hash.has_value() || !HashesEqual(*delta_hash, target->delta->hash)) {
        // A corrupt delta stops the delta path and nothing else. The full package is
        // still an option, and an update that arrives more slowly is not a failure.
        try_delta = false;
        outcome.detail = "the delta is not the delta the manifest names";
      } else {
        const auto old_bytes = ReadFile(own_archive, target->archive_size * 4, store_error);
        PatchError patch_error = PatchError::kNone;
        std::optional<std::vector<std::uint8_t>> rebuilt;
        if (old_bytes.has_value()) {
          rebuilt = ApplyPatch(*old_bytes, *delta, target->archive_size, patch_error);
        }
        if (!rebuilt.has_value()) {
          try_delta = false;
          outcome.detail = "the patch did not apply: " + std::string(Describe(patch_error));
        } else {
          archive_bytes = std::move(*rebuilt);
        }
      }
    }
  }

  // The reconstruction is 213 MiB and is not needed again either way.
  static_cast<void>(fs::remove(own_archive, ec));

  if (archive_bytes.empty()) {
    // 5, the fallback: the whole package, which is the same archive in a zstd frame.
    const auto package = fetcher.Get(target->package.url, target->package.size);
    if (!package.has_value() || package->size() != target->package.size) {
      return give_up(StageResult::kDownloadFailed, "the package could not be fetched");
    }
    outcome.downloaded += package->size();
    const auto package_hash = HashBytes(*package);
    if (!package_hash.has_value() || !HashesEqual(*package_hash, target->package.hash)) {
      return give_up(StageResult::kArtifactInvalid,
                     "the package is not the package the manifest names");
    }
    PatchError patch_error = PatchError::kNone;
    auto expanded = DecompressFrame(*package, target->archive_size, patch_error);
    if (!expanded.has_value()) {
      return give_up(StageResult::kArtifactInvalid, std::string(Describe(patch_error)));
    }
    archive_bytes = std::move(*expanded);
  } else {
    outcome.used_delta = true;
  }

  // 6: the archive's own hash, against the signed manifest, before anything opens it.
  const auto archive_hash = HashBytes(archive_bytes);
  if (!archive_hash.has_value() || !HashesEqual(*archive_hash, target->archive_hash)) {
    return give_up(StageResult::kArtifactInvalid,
                   "the rebuilt package is not the one the manifest names");
  }
  if (!WriteFileDurably(new_archive, archive_bytes, flush, store_error)) {
    return give_up(StageResult::kStagingFailed, std::string(Describe(store_error)));
  }
  archive_bytes.clear();
  archive_bytes.shrink_to_fit();

  // 7: unpacked, with every member's hash checked as it is written.
  if (!UnpackArchive(new_archive, layout.staged(), flush, store_error)) {
    return give_up(StageResult::kStagingFailed, std::string(Describe(store_error)));
  }
  static_cast<void>(RemoveTree(layout.download_dir(), store_error));

  journal.stage = Stage::kStaged;
  if (!runner.WriteJournal(journal)) {
    return give_up(StageResult::kStagingFailed, runner.last_error());
  }

  outcome.result = StageResult::kStaged;
  return outcome;
}

StartupOutcome RecoverAtStartup(const Layout& layout,
                                const FlushFn& flush,
                                bool may_touch_installation) {
  StartupOutcome outcome;
  const LoadedJournal loaded = LoadJournal(layout);
  if (!loaded.readable) {
    // Nothing. An unreadable journal is the one case where doing something is worse
    // than doing nothing, because what it might be describing is a half-done swap.
    outcome.journal_unreadable = true;
    return outcome;
  }
  outcome.journal = loaded.journal;
  FileSystemRunner runner(layout, flush);
  outcome.result = Drive(outcome.journal, runner, may_touch_installation);
  return outcome;
}

bool ConfirmLaunch(const Layout& layout, const Version& version, const FlushFn& flush) {
  return WriteLaunchFlag(layout, version, flush);
}

}  // namespace sonora::update
