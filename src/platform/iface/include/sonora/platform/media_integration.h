#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sonora::platform {

// What the operating system's media panel is told, and what it asks back.
//
// This is the second interface Sonora has. The first is the window: a person
// looking at a list and clicking. The second is the one they use without
// looking -- the play/pause key on the keyboard, the volume overlay that names
// the track, the lock screen, the panel the system shows over everything else.
// It is not a feature of the player; it is a peer of the page, and it is the
// difference between a program that plays music and one that behaves like a
// music player on that desktop.
//
// Nothing here mentions Windows. The Windows implementation is
// SystemMediaTransportControls through C++/WinRT, the macOS one is
// MPNowPlayingInfoCenter, and per ADR 0002 neither name may appear above this
// header.

enum class MediaPlaybackState {
  // The panel disappears. Not the same as stopped: closed means "this
  // application is not a media session right now".
  kClosed,
  kStopped,
  kPlaying,
  kPaused,
};

// What the panel shows. Deliberately a value type with no ids in it: the
// operating system has no idea what a library is, and this boundary is where
// that stops mattering.
struct MediaMetadata {
  std::string title;
  std::string artist;
  std::string album;
  int track_number = 0;

  // The cover, as bytes rather than as a path.
  //
  // Both systems can take a file name instead, and it would be less code here
  // and a worse boundary: the artwork Sonora has lives in the index, extracted
  // from inside the audio file, and there is no file on disk to point at. Bytes
  // also mean nothing here has to think about what happens when the picture is
  // deleted while the panel is showing it.
  std::string art_mime;
  std::vector<std::uint8_t> art;
};

struct MediaTimeline {
  std::int64_t position_ms = 0;
  std::int64_t duration_ms = 0;
};

enum class MediaCommand {
  kPlay,
  kPause,
  // The keyboard's play/pause is one key and one command: the system does not
  // know which of the two it means, and neither does this layer.
  kTogglePlayPause,
  kStop,
  kNext,
  kPrevious,
  kSeek,
};

struct MediaRequest {
  MediaCommand command = MediaCommand::kTogglePlayPause;
  // Only kSeek uses it.
  std::int64_t position_ms = 0;
};

// IMPORTANT: a request may arrive on any thread the operating system likes --
// on Windows it is a WinRT thread pool thread, and it is not the UI thread.
// Marshalling is the caller's job, because the platform layer has no idea what
// the caller's threads are. The shell posts to the CEF UI thread.
using MediaRequestFn = std::function<void(const MediaRequest&)>;

class MediaIntegration {
 public:
  virtual ~MediaIntegration() = default;

  MediaIntegration(const MediaIntegration&) = delete;
  MediaIntegration& operator=(const MediaIntegration&) = delete;

  // `native_window` is the handle the system ties the session to; Windows needs
  // one and the others ignore it. False when this build or this system has no
  // media integration, which is not a failure worth stopping for: the player
  // works, the keyboard keys do not.
  virtual bool Start(void* native_window, MediaRequestFn on_request) = 0;

  // Closes the session, so the panel stops naming an application that is on its
  // way out. Idempotent.
  virtual void Stop() = 0;

  virtual void SetMetadata(const MediaMetadata& metadata) = 0;
  virtual void SetPlaybackState(MediaPlaybackState state) = 0;
  virtual void SetTimeline(const MediaTimeline& timeline) = 0;

  // For the startup line, and to say honestly when there is nothing behind it.
  [[nodiscard]] virtual std::string description() const = 0;

 protected:
  MediaIntegration() = default;
};

[[nodiscard]] std::unique_ptr<MediaIntegration> CreateMediaIntegration();

}  // namespace sonora::platform
