#include "cef/media_session.h"

#include <chrono>
#include <filesystem>
#include <optional>
#include <utility>

#include <sonora/audio/player.h>
#include <sonora/library/library.h>

#include "cef/library_host.h"
#include "cef/player_host.h"

namespace sonora::shell {
namespace {

// Four samples a second. The policy asks for the position about once a second
// and for everything else only when it changes, so this is the rate at which
// changes are *noticed*, not the rate at which the system is called.
constexpr std::int64_t kSampleMs = 250;

[[nodiscard]] std::int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

[[nodiscard]] platform::MediaPlaybackState ToPlatform(core::PlaybackState state) {
  switch (state) {
    case core::PlaybackState::kPlaying:
    // Loading is a track that is about to be heard. To anyone watching the
    // panel it is already playing, and flickering to "stopped" for the
    // milliseconds it takes to open a file would be a lie in the other
    // direction.
    case core::PlaybackState::kLoading:
      return platform::MediaPlaybackState::kPlaying;
    case core::PlaybackState::kPaused:
      return platform::MediaPlaybackState::kPaused;
    case core::PlaybackState::kStopped:
      return platform::MediaPlaybackState::kStopped;
    case core::PlaybackState::kIdle:
      break;
  }
  return platform::MediaPlaybackState::kClosed;
}

[[nodiscard]] std::filesystem::path Utf8Path(const std::string& utf8) {
  return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

}  // namespace

ShellMediaSession::ShellMediaSession(PlayerHost& player, LibraryHost& library)
    : player_(player), library_(library) {}

ShellMediaSession::~ShellMediaSession() {
  Stop();
}

bool ShellMediaSession::Start(void* native_window) {
  integration_ = platform::CreateMediaIntegration();
  inbox_ = std::make_shared<Inbox>();
  inbox_->owner = this;

  auto inbox = inbox_;
  const bool started =
      integration_->Start(native_window, [inbox](const platform::MediaRequest& request) {
        // This runs on the system's thread. Nothing here may touch the player,
        // the library or CEF -- all three belong to the UI thread -- so the
        // only thing it does is carry the request across.
        //
        // The timer is the marshalling primitive this shell already has: a task
        // posted to the UI thread with no delay. Its handle is dropped on
        // purpose; the work is a single shot and cancelling it is what the
        // empty inbox is for.
        (void)ShellTimer::Once(0, [inbox, request] {
          if (inbox->owner != nullptr) {
            inbox->owner->Apply(request);
          }
        });
      });

  if (!started) {
    inbox_->owner = nullptr;
    inbox_.reset();
    return false;
  }

  timer_ = ShellTimer::Every(kSampleMs, [this] { Observe(); });
  Observe();
  return true;
}

void ShellMediaSession::Stop() {
  if (timer_) {
    timer_->Cancel();
    timer_ = nullptr;
  }
  if (inbox_) {
    // Before the integration goes: a media key pressed right now posts a task
    // that will run after this, and it must find nothing rather than a session
    // that is being destroyed.
    inbox_->owner = nullptr;
    inbox_.reset();
  }
  if (integration_) {
    integration_->Stop();
    integration_.reset();
  }
  policy_.Reset();
  art_key_.clear();
}

std::string ShellMediaSession::description() const {
  return integration_ ? integration_->description() : std::string("not started");
}

void ShellMediaSession::Observe() {
  if (!integration_) {
    return;
  }

  const audio::Player* player = player_.player();
  if (player == nullptr) {
    return;
  }
  const audio::Player::Snapshot snapshot = player->snapshot();

  core::NowPlaying now_playing;
  std::optional<library::Track> track;
  if (!snapshot.current_path.empty()) {
    if (library::Library* index = library_.index(); index != nullptr) {
      track = index->FindByPath(snapshot.current_path);
    }
  }

  if (track.has_value()) {
    now_playing.title = track->title;
    now_playing.artist = track->artist;
    now_playing.album = track->album;
    now_playing.art_key = track->cover_hash;
    now_playing.track_number = track->track_number;
  } else if (!snapshot.current_path.empty()) {
    // Queued by --play, or indexed under a different name: the file name is
    // what a person would recognise, and an empty panel would be worse.
    now_playing.title = [&snapshot] {
      const std::filesystem::path path = Utf8Path(snapshot.current_path);
      const std::u8string name = path.filename().u8string();
      return std::string(name.begin(), name.end());
    }();
  }
  now_playing.duration_ms = snapshot.duration_ms;

  const core::MediaUpdate update =
      policy_.Observe(now_playing, snapshot.state, snapshot.position_ms, NowMs());
  if (!update.any()) {
    return;
  }

  if (update.metadata) {
    platform::MediaMetadata metadata;
    metadata.title = now_playing.title;
    metadata.artist = now_playing.artist;
    metadata.album = now_playing.album;
    metadata.track_number = now_playing.track_number;

    // The bytes are fetched only when the cover itself changed. Every track of
    // an album shares one, so a whole album costs one read of one row.
    if (!now_playing.art_key.empty() && now_playing.art_key != art_key_) {
      if (const std::optional<library::Cover> cover = library_.Cover(now_playing.art_key);
          cover.has_value()) {
        metadata.art_mime = cover->mime;
        metadata.art = cover->bytes;
      }
    }
    art_key_ = now_playing.art_key;

    integration_->SetMetadata(metadata);
  }

  if (update.state) {
    integration_->SetPlaybackState(ToPlatform(snapshot.state));
  }

  if (update.timeline) {
    platform::MediaTimeline timeline;
    timeline.position_ms = snapshot.position_ms;
    timeline.duration_ms = snapshot.duration_ms;
    integration_->SetTimeline(timeline);
  }
}

void ShellMediaSession::Apply(const platform::MediaRequest& request) {
  audio::Player* player = player_.player();
  if (player == nullptr) {
    return;
  }

  switch (request.command) {
    case platform::MediaCommand::kPlay:
      player->Play();
      break;
    case platform::MediaCommand::kPause:
      player->Pause();
      break;
    case platform::MediaCommand::kTogglePlayPause:
      // The keyboard's one key. What it means depends on what is happening, and
      // this is the only place that knows.
      if (player->snapshot().state == core::PlaybackState::kPlaying) {
        player->Pause();
      } else {
        player->Play();
      }
      break;
    case platform::MediaCommand::kStop:
      player->Stop();
      break;
    case platform::MediaCommand::kNext:
      player->Next();
      break;
    case platform::MediaCommand::kPrevious:
      player->Previous();
      break;
    case platform::MediaCommand::kSeek:
      player->SeekMs(request.position_ms);
      break;
  }

  // Straight away rather than at the next tick: somebody who pressed a key is
  // watching the panel, and 250 ms of the old state is exactly long enough to
  // look like the key did nothing.
  Observe();
}

}  // namespace sonora::shell
