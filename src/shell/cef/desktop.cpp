#include "cef/desktop.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <optional>
#include <unordered_set>
#include <utility>

#include <sonora/audio/player.h>
#include <sonora/core/deep_link.h>
#include <sonora/library/library.h>

#include "cef/library_host.h"
#include "cef/player_host.h"

namespace sonora::shell {
namespace {

// The tray tooltip and the play history do not need to be sampled ten times a
// second. Once is plenty, and a timer that wakes up rarely is a timer nobody
// has to defend in a battery-life discussion.
constexpr std::int64_t kPollMs = 1000;

// How many albums the jump list shows. Windows decides how many it will
// actually display and BeginList says so; this is the bound on how far back the
// durable store is read, and it is generous because several recent plays are
// usually the same album.
constexpr int kRecentPathsToRead = 64;
constexpr std::size_t kMaxJumpListEntries = 8;

[[nodiscard]] std::int64_t NowMs() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

[[nodiscard]] std::filesystem::path Utf8Path(const std::string& utf8) {
  return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

}  // namespace

DesktopIntegration::DesktopIntegration(PlayerHost& player, LibraryHost& library)
    : player_(player), library_(library) {}

DesktopIntegration::~DesktopIntegration() {
  Stop();
}

bool DesktopIntegration::Start(void* native_window,
                               const std::filesystem::path& store_path,
                               Callbacks callbacks) {
  callbacks_ = std::move(callbacks);

  try {
    store_ = std::make_unique<state::Store>(store_path);
  } catch (const state::StateError& error) {
    // Refusing to carry on is the right answer here, and it is the opposite of
    // what the library index gets. The index is a cache: when it will not open,
    // the capability goes off and Sonora still plays music. This store hands
    // out the ids that go into the operating system, and running without it
    // would mean either no jump list -- fine -- or, worse, one built on ids
    // from somewhere else.
    std::fprintf(stderr, "desktop: %s\n", error.what());
    return false;
  }

  // Created even when Start() below fails: the object that does nothing is
  // still an object, and description() is what tells the startup log which one
  // this is.
  integration_ = platform::CreateShellIntegration();
  inbox_ = std::make_shared<Inbox>();
  inbox_->owner = this;

  auto inbox = inbox_;
  const bool started =
      integration_->Start(native_window, [inbox](platform::ShellCommand command) {
        // The tray's menu runs inside TrackPopupMenu on the UI thread, so this
        // is already the right thread on Windows today. It is posted anyway --
        // see the note in single_instance.h about assumptions that hold on the
        // platform they were written on.
        (void)ShellTimer::Once(0, [inbox, command] {
          if (inbox->owner != nullptr) {
            inbox->owner->ApplyCommand(command);
          }
        });
      });

  if (started) {
    RefreshJumpList();
  }

  timer_ = ShellTimer::Every(kPollMs, [this] { Poll(); });
  Poll();
  return true;
}

void DesktopIntegration::Stop() {
  if (timer_) {
    timer_->Cancel();
    timer_ = nullptr;
  }
  if (inbox_) {
    inbox_->owner = nullptr;
    inbox_.reset();
  }
  if (integration_) {
    integration_->Stop();
    integration_.reset();
  }
  store_.reset();
  callbacks_ = Callbacks{};
}

std::string DesktopIntegration::description() const {
  return integration_ ? integration_->description() : std::string("not started");
}

void DesktopIntegration::Activate(const std::vector<std::string>& arguments) {
  auto inbox = inbox_;
  if (!inbox) {
    return;
  }
  (void)ShellTimer::Once(0, [inbox, arguments] {
    if (inbox->owner != nullptr) {
      inbox->owner->ApplyActivation(arguments);
    }
  });
}

void DesktopIntegration::ApplyActivation(const std::vector<std::string>& arguments) {
  // Raised first, and whatever the arguments turn out to mean. Somebody who
  // clicked a link or launched Sonora a second time asked for Sonora to be in
  // front of them; a link that names a track nobody has any more is not a
  // reason to leave them looking at the browser they clicked it in.
  if (callbacks_.raise_window) {
    callbacks_.raise_window();
  }

  audio::Player* player = player_.player();
  library::Library* index = library_.index();
  if (player == nullptr || index == nullptr || !store_) {
    return;
  }

  for (const std::string& argument : arguments) {
    const core::DeepLink link = core::ParseDeepLink(argument);
    if (!link.valid()) {
      continue;
    }

    // Three lookups, and the order is the whole of ADR 0008: a durable id
    // names a path, the path names a row of the index, and only then is there
    // anything to play. A link whose file was deleted stops at the second step
    // with nothing happening, which is the correct amount of drama.
    const std::optional<std::string> path = store_->PathForId(link.track_id);
    if (!path.has_value()) {
      std::printf("desktop: link %s names nothing this installation knows\n", argument.c_str());
      continue;
    }
    const std::optional<library::Track> track = index->FindByPath(*path);
    if (!track.has_value()) {
      std::printf("desktop: link %s names a file the index no longer has\n", argument.c_str());
      continue;
    }

    std::vector<library::Track> to_play;
    if (link.kind == core::DeepLinkKind::kAlbum) {
      to_play = index->AlbumTracks(track->album, track->album_artist);
    }
    if (to_play.empty()) {
      to_play.push_back(*track);
    }

    player->ClearQueue();
    for (const library::Track& queued : to_play) {
      player->Enqueue(Utf8Path(queued.path));
    }
    player->Play();
    std::printf("desktop: %s -> %zu track(s)\n", argument.c_str(), to_play.size());
    return;  // one link per activation; a command line with two is not a thing
  }
}

void DesktopIntegration::ApplyCommand(platform::ShellCommand command) {
  audio::Player* player = player_.player();

  switch (command) {
    case platform::ShellCommand::kShowWindow:
      if (callbacks_.raise_window) {
        callbacks_.raise_window();
      }
      break;

    case platform::ShellCommand::kTogglePlayPause:
      if (player != nullptr) {
        if (player->snapshot().state == core::PlaybackState::kPlaying) {
          player->Pause();
        } else {
          player->Play();
        }
      }
      break;

    case platform::ShellCommand::kNext:
      if (player != nullptr) {
        player->Next();
      }
      break;

    case platform::ShellCommand::kPrevious:
      if (player != nullptr) {
        player->Previous();
      }
      break;

    case platform::ShellCommand::kQuit:
      if (callbacks_.quit) {
        callbacks_.quit();
      }
      break;
  }

  // The tray's menu says Play or Pause depending on what it will do next, and
  // a menu that describes the state it just left is a menu that looks broken.
  Poll();
}

void DesktopIntegration::Poll() {
  const audio::Player* player = player_.player();
  if (player == nullptr || !integration_) {
    return;
  }
  const audio::Player::Snapshot snapshot = player->snapshot();

  std::string tooltip = "Sonora";
  if (!snapshot.current_path.empty()) {
    if (library::Library* index = library_.index(); index != nullptr) {
      if (const std::optional<library::Track> track = index->FindByPath(snapshot.current_path);
          track.has_value()) {
        tooltip = track->artist.empty() ? track->title : track->artist + " - " + track->title;
      }
    }
  }

  const bool playing = snapshot.state == core::PlaybackState::kPlaying;
  if (tooltip != last_tooltip_ || playing != last_playing_) {
    platform::ShellState state;
    state.tooltip = tooltip;
    state.playing = playing;
    integration_->SetState(state);
    last_tooltip_ = tooltip;
    last_playing_ = playing;
  }

  // A play is recorded when a track *starts*, not when it finishes. That is a
  // choice, and the honest one for what this feeds: a jump list of what
  // somebody was listening to. Waiting for the end would mean an album skipped
  // through leaves no trace, and half the point is finding your way back to
  // what you had on an hour ago.
  if (playing && !snapshot.current_path.empty() && snapshot.current_path != recorded_path_) {
    RecordPlay(snapshot.current_path);
  }
}

void DesktopIntegration::RecordPlay(const std::string& path) {
  if (!store_) {
    return;
  }
  recorded_path_ = path;
  try {
    store_->NotePlayed(path, NowMs());
  } catch (const state::StateError& error) {
    // A write that failed costs a jump-list entry. It is not worth interrupting
    // playback over, and it is worth saying out loud once.
    std::fprintf(stderr, "desktop: could not record a play: %s\n", error.what());
    return;
  }
  RefreshJumpList();
}

void DesktopIntegration::RefreshJumpList() {
  library::Library* index = library_.index();
  if (!store_ || !integration_ || index == nullptr) {
    return;
  }

  std::vector<platform::JumpListEntry> entries;
  std::unordered_set<std::string> seen;

  // Grouped here rather than in SQL, and for a reason that is the shape of the
  // whole week: the recency lives in one store and the album names live in the
  // other, so there is no query that can see both. The join is a loop over
  // sixty-four rows, which is the right amount of machinery for a context menu.
  for (const state::PlayRecord& record : store_->RecentlyPlayed(kRecentPathsToRead)) {
    const std::optional<library::Track> track = index->FindByPath(record.path);
    if (!track.has_value() || track->album.empty()) {
      continue;
    }
    const std::string key = track->album_artist + '\x1f' + track->album;
    if (!seen.insert(key).second) {
      continue;
    }

    platform::JumpListEntry entry;
    entry.title = track->album;
    entry.description = track->album_artist.empty() ? track->artist : track->album_artist;
    // The durable id, never the index's: this string is stored by the shell and
    // handed back to some later version of this program. See ADR 0008.
    entry.arguments = core::MakeDeepLink(core::DeepLinkKind::kAlbum, record.id);
    entries.push_back(std::move(entry));

    if (entries.size() >= kMaxJumpListEntries) {
      break;
    }
  }

  integration_->SetJumpList(entries);
}

}  // namespace sonora::shell
