#include "cef/handlers.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <sonora/audio/player.h>
#include <sonora/bridge/protocol.h>
#include <sonora/core/version.h>
#include <sonora/library/library.h>

#include "cef/event_channel.h"
#include "cef/library_host.h"
#include "cef/player_host.h"
#include "cef/shell_metrics.h"
#include "include/cef_version.h"

namespace sonora::shell {
namespace {

// cef_version.h gives the parts and the stringify helper, but not the assembled
// Chromium version.
#define SONORA_CHROMIUM_VERSION                                                                \
  MAKE_STRING(CHROME_VERSION_MAJOR)                                                            \
  "." MAKE_STRING(CHROME_VERSION_MINOR) "." MAKE_STRING(CHROME_VERSION_BUILD) "." MAKE_STRING( \
      CHROME_VERSION_PATCH)

// A page asking for a million copies of a string is a page asking the browser
// process to allocate a gigabyte on its UI thread. The limit is the point.
constexpr std::int64_t kMaxEchoRepeat = 64;

// Week 6 had a ValidateTrackPath here: the page sent a file path, and this
// function tried to rule out the mistakes -- a relative path, something carrying
// a URL scheme -- while admitting in its own comment that it was not a
// permission model, because a compromised renderer could still name any file the
// user can read.
//
// It is gone, and nothing replaced it. The page sends a library id; an id is
// either a row in the index or it is not, and the path comes out of that row.
// There is no string from the renderer that reaches the filesystem any more, so
// there is nothing left to validate -- which is a better outcome than a stricter
// check, and the reason week 7 was worth doing in this order.

// A page may not ask for the whole library in one answer. The limit is not about
// the database, which would happily return fifty thousand rows: it is about the
// bridge, where every row is serialised to JSON, copied across a process
// boundary and parsed again.
constexpr std::int64_t kMaxRows = 5000;
constexpr std::int64_t kDefaultListRows = 500;
constexpr std::int64_t kDefaultSearchRows = 200;

[[nodiscard]] std::int64_t Rows(const std::optional<std::int64_t>& asked,
                                std::int64_t fallback) {
  const std::int64_t value = asked.value_or(fallback);
  if (value < 0) {
    throw bridge::BridgeError(bridge::ErrorCode::kInvalidParams, "limit must not be negative");
  }
  return std::min(value, kMaxRows);
}

[[nodiscard]] std::filesystem::path Utf8Path(const std::string& utf8) {
  return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

[[nodiscard]] std::string PathToUtf8(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return std::string(text.begin(), text.end());
}

// Every transport command needs the same two lines, and forgetting them is a
// null dereference on a machine with no sound card.
[[nodiscard]] audio::Player& RequirePlayer(PlayerHost& host) {
  audio::Player* player = host.player();
  if (player == nullptr) {
    throw bridge::BridgeError(bridge::ErrorCode::kUnavailable, "no audio device is open");
  }
  return *player;
}

}  // namespace

ShellHandlers::ShellHandlers(const bridge::CapabilityRegistry& capabilities,
                             const ShellMetrics& metrics,
                             const EventChannel& events,
                             PlayerHost& player,
                             LibraryHost& library)
    : capabilities_(capabilities),
      metrics_(metrics),
      events_(events),
      player_(player),
      library_(library) {}

library::Library& ShellHandlers::RequireIndex() {
  library::Library* index = library_.index();
  if (index == nullptr) {
    throw bridge::BridgeError(bridge::ErrorCode::kUnavailable,
                              "the library index is not available");
  }
  return *index;
}

bridge::ShellGetVersionResult ShellHandlers::ShellGetVersion(
    const bridge::ShellGetVersionParams& params) {
  (void)params;

  bridge::ShellGetVersionResult result;
  result.version = core::kVersion;
  result.gitDescribe = core::kGitDescribe;
  result.cefVersion = CEF_VERSION;
  result.chromiumVersion = SONORA_CHROMIUM_VERSION;
  return result;
}

bridge::ShellEchoResult ShellHandlers::ShellEcho(const bridge::ShellEchoParams& params) {
  // The generated parser has already checked the shapes. What is left is what
  // only this method knows: what the values are allowed to mean.
  if (params.message.empty()) {
    throw bridge::BridgeError(bridge::ErrorCode::kInvalidParams, "message must not be empty");
  }

  const std::int64_t repeat = params.repeat.value_or(1);
  if (repeat < 1 || repeat > kMaxEchoRepeat) {
    throw bridge::BridgeError(bridge::ErrorCode::kInvalidParams,
                              "repeat must be between 1 and " + std::to_string(kMaxEchoRepeat));
  }

  std::string repeated;
  repeated.reserve(params.message.size() * static_cast<std::size_t>(repeat));
  for (std::int64_t i = 0; i < repeat; ++i) {
    repeated += params.message;
  }

  bridge::ShellEchoResult result;
  result.lengthBytes = static_cast<std::int64_t>(repeated.size());
  result.message = std::move(repeated);
  return result;
}

bridge::ShellGetCapabilitiesResult ShellHandlers::ShellGetCapabilities(
    const bridge::ShellGetCapabilitiesParams& params) {
  (void)params;

  // Straight off the registry, which was built from the generated table. This
  // answer cannot disagree with what the schema declares or with what this run
  // has switched off, because there is no second list to keep in step.
  bridge::ShellGetCapabilitiesResult result;
  result.protocolVersion = bridge::kProtocolVersion;
  for (const bridge::CapabilityState& state : capabilities_.all()) {
    bridge::Capability capability;
    capability.name = state.name;
    capability.version = state.version;
    capability.enabled = state.enabled;
    result.capabilities.push_back(std::move(capability));
  }
  return result;
}

// ---------------------------------------------------------------------------
// player
// ---------------------------------------------------------------------------
//
// Every command returns once the player has been told, not once the sound has
// changed. The two are different moments -- a seek takes as long as the decode
// thread needs -- and pretending otherwise would mean blocking a bridge call on
// audio, which is the one thing this whole design is arranged to avoid. The
// player.state event is what says what actually happened.

bridge::PlayerGetStateResult ShellHandlers::PlayerGetState(
    const bridge::PlayerGetStateParams& params) {
  (void)params;
  bridge::PlayerGetStateResult result;
  result.player = player_.State();
  return result;
}

bridge::PlayerEnqueueResult ShellHandlers::PlayerEnqueue(
    const bridge::PlayerEnqueueParams& params) {
  audio::Player& player = RequirePlayer(player_);
  if (params.trackIds.empty()) {
    throw bridge::BridgeError(bridge::ErrorCode::kInvalidParams, "trackIds must not be empty");
  }
  if (static_cast<std::int64_t>(params.trackIds.size()) > kMaxRows) {
    throw bridge::BridgeError(
        bridge::ErrorCode::kInvalidParams,
        "trackIds must hold at most " + std::to_string(kMaxRows) + " ids");
  }

  // Resolved in one query, before anything is queued. All or nothing on purpose:
  // queueing eleven tracks of a twelve-track album and reporting an error is a
  // state nobody can reason about, least of all the page that has to decide
  // whether to retry.
  const std::vector<library::Track> tracks = RequireIndex().GetMany(params.trackIds);
  if (tracks.size() != params.trackIds.size()) {
    throw bridge::BridgeError(bridge::ErrorCode::kInvalidParams,
                              "the library has no track with that id");
  }

  if (params.replace.value_or(false)) {
    player.ClearQueue();
  }
  for (const library::Track& track : tracks) {
    player.Enqueue(Utf8Path(track.path));
  }

  // The queue size the page is told is the one it will see: Enqueue is applied
  // by the decode thread, so reading it back from the snapshot here would report
  // the value from before this call.
  bridge::PlayerEnqueueResult result;
  const int queued = static_cast<int>(tracks.size());
  result.queueSize =
      params.replace.value_or(false) ? queued : player.snapshot().queue_size + queued;
  return result;
}

bridge::PlayerGetQueueResult ShellHandlers::PlayerGetQueue(
    const bridge::PlayerGetQueueParams& params) {
  (void)params;
  audio::Player& player = RequirePlayer(player_);

  // The player's queue is paths, because the player knows nothing about a
  // library. Turning them back into titles is the index's job, and going through
  // FindByPath means a file queued by --play -- which never came from the index
  // -- still draws as something rather than as a blank row.
  library::Library* index = library_.index();

  bridge::PlayerGetQueueResult result;
  int position = 0;
  for (const std::string& path : player.queue_paths()) {
    bridge::QueueEntry entry;
    entry.index = position++;

    const std::optional<library::Track> track =
        index != nullptr ? index->FindByPath(path) : std::nullopt;
    if (track.has_value()) {
      entry.trackId = track->id;
      entry.title = track->title;
      entry.artist = track->artist;
      entry.durationMs = track->duration_ms;
      entry.artUrl = LibraryHost::ArtUrl(track->cover_hash);
    } else {
      // Not in the index: the file name is the only honest thing to show, and it
      // is what the person would recognise.
      const std::string name = PathToUtf8(Utf8Path(path).filename());
      entry.title = name.empty() ? path : name;
    }
    result.entries.push_back(std::move(entry));
  }
  return result;
}

bridge::PlayerClearQueueResult ShellHandlers::PlayerClearQueue(
    const bridge::PlayerClearQueueParams& params) {
  (void)params;
  RequirePlayer(player_).ClearQueue();
  return {};
}

bridge::PlayerPlayResult ShellHandlers::PlayerPlay(const bridge::PlayerPlayParams& params) {
  (void)params;
  RequirePlayer(player_).Play();
  return {};
}

bridge::PlayerPauseResult ShellHandlers::PlayerPause(const bridge::PlayerPauseParams& params) {
  (void)params;
  RequirePlayer(player_).Pause();
  return {};
}

bridge::PlayerStopResult ShellHandlers::PlayerStop(const bridge::PlayerStopParams& params) {
  (void)params;
  RequirePlayer(player_).Stop();
  return {};
}

bridge::PlayerNextResult ShellHandlers::PlayerNext(const bridge::PlayerNextParams& params) {
  (void)params;
  RequirePlayer(player_).Next();
  return {};
}

bridge::PlayerPreviousResult ShellHandlers::PlayerPrevious(
    const bridge::PlayerPreviousParams& params) {
  (void)params;
  RequirePlayer(player_).Previous();
  return {};
}

bridge::PlayerJumpToResult ShellHandlers::PlayerJumpTo(
    const bridge::PlayerJumpToParams& params) {
  // Out of range is the player's business, not this method's: it holds the queue
  // and this call is asynchronous anyway, so a bounds check here would be a check
  // against a queue size that may already have changed.
  RequirePlayer(player_).PlayTrack(static_cast<int>(params.index));
  return {};
}

bridge::PlayerSeekResult ShellHandlers::PlayerSeek(const bridge::PlayerSeekParams& params) {
  if (params.positionMs < 0) {
    throw bridge::BridgeError(bridge::ErrorCode::kInvalidParams,
                              "positionMs must not be negative");
  }
  RequirePlayer(player_).SeekMs(params.positionMs);
  return {};
}

bridge::PlayerSetVolumeResult ShellHandlers::PlayerSetVolume(
    const bridge::PlayerSetVolumeParams& params) {
  // Clamped rather than rejected: a slider that sends 1.0000001 because of
  // floating point is not a caller mistake worth an error.
  RequirePlayer(player_).SetVolume(static_cast<float>(params.level));
  return {};
}

// ---------------------------------------------------------------------------
// library
// ---------------------------------------------------------------------------
//
// Every read here is answered synchronously, on the CEF UI thread, straight out
// of SQLite -- these are indexed queries over tens of thousands of rows, which is
// microseconds, and a task hop would cost more than the query. The one thing that
// is not answered synchronously is the scan, because a scan is minutes.

bridge::LibraryGetStatusResult ShellHandlers::LibraryGetStatus(
    const bridge::LibraryGetStatusParams& params) {
  (void)params;
  bridge::LibraryGetStatusResult result;
  result.library = library_.Status();
  return result;
}

bridge::LibraryScanResult ShellHandlers::LibraryScan(const bridge::LibraryScanParams& params) {
  // Called for the check it makes, not for the index it returns: scanning is the
  // host's business, not the index's. The cast is what says that on purpose --
  // [[nodiscard]] is right to ask, and MSVC is right to warn.
  static_cast<void>(RequireIndex());

  const std::filesystem::path root =
      params.path.has_value() ? Utf8Path(*params.path) : std::filesystem::path();
  if (params.path.has_value() && root.empty()) {
    throw bridge::BridgeError(bridge::ErrorCode::kInvalidParams,
                              "path must not be empty; leave it out to rescan");
  }

  // Note what this does not do: it does not wait, and it does not report what the
  // scan found. It says whether one started. Everything else arrives as
  // library.status, which is the only shape that works for something that takes
  // minutes and that the page has to stay responsive during.
  bridge::LibraryScanResult result;
  result.started = library_.StartScan(root);
  result.root = library_.Status().root;
  return result;
}

bridge::LibraryListTracksResult ShellHandlers::LibraryListTracks(
    const bridge::LibraryListTracksParams& params) {
  library::Library& index = RequireIndex();
  const std::int64_t offset = params.offset.value_or(0);
  if (offset < 0) {
    throw bridge::BridgeError(bridge::ErrorCode::kInvalidParams, "offset must not be negative");
  }

  bridge::LibraryListTracksResult result;
  for (const library::Track& track :
       index.ListTracks(Rows(params.limit, kDefaultListRows), offset)) {
    result.tracks.push_back(LibraryHost::ToBridge(track));
  }
  // The total, so a page can size a scrollbar for fifty thousand rows while
  // holding forty of them.
  result.total = index.TrackCount();
  return result;
}

bridge::LibraryListAlbumsResult ShellHandlers::LibraryListAlbums(
    const bridge::LibraryListAlbumsParams& params) {
  (void)params;
  bridge::LibraryListAlbumsResult result;
  for (const library::AlbumSummary& album : RequireIndex().ListAlbums()) {
    result.albums.push_back(LibraryHost::ToBridge(album));
  }
  return result;
}

bridge::LibraryListArtistsResult ShellHandlers::LibraryListArtists(
    const bridge::LibraryListArtistsParams& params) {
  (void)params;
  bridge::LibraryListArtistsResult result;
  result.artists = RequireIndex().ListArtists();
  return result;
}

bridge::LibraryAlbumTracksResult ShellHandlers::LibraryAlbumTracks(
    const bridge::LibraryAlbumTracksParams& params) {
  bridge::LibraryAlbumTracksResult result;
  for (const library::Track& track :
       RequireIndex().AlbumTracks(params.album, params.albumArtist)) {
    result.tracks.push_back(LibraryHost::ToBridge(track));
  }
  return result;
}

bridge::LibraryArtistTracksResult ShellHandlers::LibraryArtistTracks(
    const bridge::LibraryArtistTracksParams& params) {
  bridge::LibraryArtistTracksResult result;
  for (const library::Track& track : RequireIndex().ArtistTracks(params.artist)) {
    result.tracks.push_back(LibraryHost::ToBridge(track));
  }
  return result;
}

bridge::LibrarySearchResult ShellHandlers::LibrarySearch(
    const bridge::LibrarySearchParams& params) {
  // params.query goes straight in. Not because it is trusted -- it is whatever
  // somebody typed -- but because Library::Search treats it as text rather than
  // as syntax, which is where that decision belongs: one place, tested, rather
  // than a sanitiser here that the next caller forgets.
  bridge::LibrarySearchResult result;
  for (const library::Track& track :
       RequireIndex().Search(params.query, Rows(params.limit, kDefaultSearchRows))) {
    result.tracks.push_back(LibraryHost::ToBridge(track));
  }
  return result;
}

bridge::DiagnosticsGetMetricsResult ShellHandlers::DiagnosticsGetMetrics(
    const bridge::DiagnosticsGetMetricsParams& params) {
  (void)params;

  const bridge::EventCoalescer::Stats& events = events_.stats();

  bridge::DiagnosticsGetMetricsResult result;
  result.uptimeMs = metrics_.uptime_ms();
  result.queriesHandled = metrics_.queries_handled();
  result.eventsPosted = events.posted;
  result.eventsDelivered = events.delivered;
  result.eventsCoalesced = events.coalesced;
  return result;
}

}  // namespace sonora::shell
