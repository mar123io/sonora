#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sonora/update/filestore.h"
#include "sonora/update/manifest.h"
#include "sonora/update/signature.h"
#include "sonora/update/version.h"

namespace sonora::update {

// Getting bytes from a url, and the only thing in this subsystem that touches a
// network.
//
// An interface rather than a function for the same reason Runner is one: the
// end-to-end tests drive the whole of the logic below with a Fetcher that reads from
// a directory, and everything they exercise -- the signature, the choosing, the
// reconstruction, the patch, the hashes, the unpack, the journal, the swap, the
// rollback -- is then the same code a user runs. It is not a back door: the
// production binary constructs the real one, and there is no argument, environment
// variable or file that can make it construct another.
class Fetcher {
 public:
  virtual ~Fetcher();

  // `max_bytes` comes from the signed manifest wherever there is one to come from. A
  // response larger than it is refused rather than truncated: truncating would hand
  // the hash check something that was never the artefact.
  [[nodiscard]] virtual std::optional<std::vector<std::uint8_t>> Get(
      std::string_view url,
      std::uint64_t max_bytes) = 0;
};

struct UpdateConfig {
  std::string manifest_url;
  std::string signature_url;
  std::string platform = "win-x64";
  // Defaults to ReleaseKeys(); a test passes its own.
  std::span<const PublicKey> keys;
};

enum class StageResult {
  kUpToDate,
  kStaged,  // Sonora.new is complete, verified, and the journal says so
  kBusy,    // an update is already in flight; this one is not started
  kJournalUnreadable,
  kManifestUnavailable,
  kSignatureInvalid,
  kManifestInvalid,
  kDownloadFailed,
  kArtifactInvalid,  // a hash or a size that does not match the signed manifest
  kNoRoom,
  kStagingFailed,
};

[[nodiscard]] std::string_view Describe(StageResult result);

struct StageOutcome {
  StageResult result = StageResult::kUpToDate;
  Version version;               // the version staged, when one was
  bool used_delta = false;       // whether the patch path was taken
  std::uint64_t downloaded = 0;  // bytes off the network
  std::string detail;            // for the log, never for a decision
  // This installation's rollout bucket for the version it was offered, or -1 when there
  // was no id to compute one from. Reported so that the about panel can show it: a
  // mechanism whose effect nobody can observe is indistinguishable from a broken one.
  int bucket = -1;
};

// Checks for an update and, if there is one, leaves a verified tree in
// <root>/Sonora.new with the journal saying `staged`. It moves nothing: putting the
// staged tree in place is ADR 0011's business and happens at the application's exit.
//
// The order of operations is the security argument, and it is worth reading as a
// list:
//
//   1. the manifest and its signature are fetched;
//   2. the signature is verified over the manifest's bytes, before any parse;
//   3. the manifest is parsed, and it names sizes and hashes for everything else;
//   4. the client's own installation is packed into an archive and, if the manifest
//      still lists the running version, its hash is checked against it -- so the
//      delta path is only taken when the thing being patched from is known to be
//      the right bytes;
//   5. the delta or the package is fetched, capped at the size the manifest names;
//   6. the resulting archive is hashed and compared against the manifest before it
//      is opened;
//   7. it is unpacked, and every member's hash is checked as it is written.
//
// Nothing is parsed before it has been authenticated, and nothing is written into
// place before it has been hashed twice.
[[nodiscard]] StageOutcome CheckAndStage(const Layout& layout,
                                         Fetcher& fetcher,
                                         const UpdateConfig& config,
                                         const Version& current,
                                         const FlushFn& flush);

// What the application does in its first ten lines: read the journal, finish or undo
// whatever is in flight, and say whether the rest has to happen in a process that is
// not running from the installation directory.
struct StartupOutcome {
  DriveResult result = DriveResult::kDone;
  Journal journal;
  bool journal_unreadable = false;
};

[[nodiscard]] StartupOutcome RecoverAtStartup(const Layout& layout,
                                              const FlushFn& flush,
                                              bool may_touch_installation);

// What the application does once its window is up and the UI has loaded: the
// milestone that ADR 0011 uses instead of a timer.
[[nodiscard]] bool ConfirmLaunch(const Layout& layout,
                                 const Version& version,
                                 const FlushFn& flush);

}  // namespace sonora::update
