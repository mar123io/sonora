#include <sonora/platform/media_integration.h>

#import <AppKit/AppKit.h>
#import <MediaPlayer/MediaPlayer.h>

#include <memory>
#include <mutex>
#include <utility>

// The macOS side of the media integration: MPNowPlayingInfoCenter for what the
// system shows, MPRemoteCommandCenter for what it sends back.
//
// **Written, compiled by CI, never run.** No Mac has executed a line of this
// file. It is here because the shape of the port is the valuable part -- the
// same interface, a second operating system with entirely different vocabulary
// for the same five ideas -- and because a backend that stops compiling is a
// backend somebody notices. It is not here to be claimed as working, and the
// README says the same thing in the same words.
//
// The parts most likely to be wrong on first contact with real hardware: the
// artwork's size callback, whether the commands arrive without a bundled
// application, and the exact playback rate the system wants while paused.

namespace sonora::platform {
namespace {

struct Shared {
  std::mutex mutex;
  MediaRequestFn on_request;

  void Dispatch(MediaCommand command, std::int64_t position_ms = 0) {
    MediaRequestFn handler;
    {
      const std::lock_guard<std::mutex> lock(mutex);
      handler = on_request;
    }
    if (handler) {
      MediaRequest request;
      request.command = command;
      request.position_ms = position_ms;
      handler(request);
    }
  }
};

[[nodiscard]] NSString* ToNsString(const std::string& utf8) {
  NSString* value = [NSString stringWithUTF8String:utf8.c_str()];
  return value != nil ? value : @"";
}

class MacMediaIntegration final : public MediaIntegration {
 public:
  ~MacMediaIntegration() override { Stop(); }

  bool Start(void* native_window, MediaRequestFn on_request) override {
    // Unused: on macOS the now-playing session belongs to the process, not to a
    // window. The parameter stays in the interface because Windows needs it,
    // and an interface shaped by its most demanding implementation is cheaper
    // than two interfaces.
    (void)native_window;

    shared_ = std::make_shared<Shared>();
    {
      const std::lock_guard<std::mutex> lock(shared_->mutex);
      shared_->on_request = std::move(on_request);
    }

    MPRemoteCommandCenter* center = [MPRemoteCommandCenter sharedCommandCenter];
    auto shared = shared_;

    [center.playCommand addTargetWithHandler:^(MPRemoteCommandEvent*) {
      shared->Dispatch(MediaCommand::kPlay);
      return MPRemoteCommandHandlerStatusSuccess;
    }];
    [center.pauseCommand addTargetWithHandler:^(MPRemoteCommandEvent*) {
      shared->Dispatch(MediaCommand::kPause);
      return MPRemoteCommandHandlerStatusSuccess;
    }];
    [center.togglePlayPauseCommand addTargetWithHandler:^(MPRemoteCommandEvent*) {
      shared->Dispatch(MediaCommand::kTogglePlayPause);
      return MPRemoteCommandHandlerStatusSuccess;
    }];
    [center.stopCommand addTargetWithHandler:^(MPRemoteCommandEvent*) {
      shared->Dispatch(MediaCommand::kStop);
      return MPRemoteCommandHandlerStatusSuccess;
    }];
    [center.nextTrackCommand addTargetWithHandler:^(MPRemoteCommandEvent*) {
      shared->Dispatch(MediaCommand::kNext);
      return MPRemoteCommandHandlerStatusSuccess;
    }];
    [center.previousTrackCommand addTargetWithHandler:^(MPRemoteCommandEvent*) {
      shared->Dispatch(MediaCommand::kPrevious);
      return MPRemoteCommandHandlerStatusSuccess;
    }];
    [center.changePlaybackPositionCommand addTargetWithHandler:^(MPRemoteCommandEvent* event) {
      auto* position = (MPChangePlaybackPositionCommandEvent*)event;
      shared->Dispatch(MediaCommand::kSeek,
                       static_cast<std::int64_t>(position.positionTime * 1000.0));
      return MPRemoteCommandHandlerStatusSuccess;
    }];

    started_ = true;
    return true;
  }

  void Stop() override {
    if (shared_) {
      const std::lock_guard<std::mutex> lock(shared_->mutex);
      shared_->on_request = nullptr;
    }
    if (!started_) {
      return;
    }

    MPRemoteCommandCenter* center = [MPRemoteCommandCenter sharedCommandCenter];
    [center.playCommand removeTarget:nil];
    [center.pauseCommand removeTarget:nil];
    [center.togglePlayPauseCommand removeTarget:nil];
    [center.stopCommand removeTarget:nil];
    [center.nextTrackCommand removeTarget:nil];
    [center.previousTrackCommand removeTarget:nil];
    [center.changePlaybackPositionCommand removeTarget:nil];

    MPNowPlayingInfoCenter.defaultCenter.nowPlayingInfo = nil;
    MPNowPlayingInfoCenter.defaultCenter.playbackState = MPNowPlayingPlaybackStateStopped;

    started_ = false;
    shared_.reset();
  }

  void SetMetadata(const MediaMetadata& metadata) override {
    if (!started_) {
      return;
    }

    NSMutableDictionary* info = [NSMutableDictionary dictionary];
    info[MPMediaItemPropertyTitle] = ToNsString(metadata.title);
    info[MPMediaItemPropertyArtist] = ToNsString(metadata.artist);
    info[MPMediaItemPropertyAlbumTitle] = ToNsString(metadata.album);
    if (metadata.track_number > 0) {
      info[MPMediaItemPropertyAlbumTrackNumber] = @(metadata.track_number);
    }

    if (!metadata.art.empty()) {
      NSData* data = [NSData dataWithBytes:metadata.art.data() length:metadata.art.size()];
      NSImage* image = [[NSImage alloc] initWithData:data];
      if (image != nil) {
        // The system asks for the artwork at whatever size it needs. Returning
        // the same image for every request is what every example does and is
        // almost certainly where a real Mac would first disagree with this file.
        MPMediaItemArtwork* artwork = [[MPMediaItemArtwork alloc] initWithBoundsSize:image.size
                                                                      requestHandler:^(CGSize) {
                                                                        return image;
                                                                      }];
        info[MPMediaItemPropertyArtwork] = artwork;
      }
    }

    // The timeline values are part of the same dictionary on this platform, so
    // they are carried over rather than cleared -- otherwise every track change
    // would blank the progress bar until the next timeline push.
    info[MPMediaItemPropertyPlaybackDuration] = @(duration_ms_ / 1000.0);
    info[MPNowPlayingInfoPropertyElapsedPlaybackTime] = @(position_ms_ / 1000.0);
    info[MPNowPlayingInfoPropertyPlaybackRate] = @(playing_ ? 1.0 : 0.0);

    MPNowPlayingInfoCenter.defaultCenter.nowPlayingInfo = info;
  }

  void SetPlaybackState(MediaPlaybackState state) override {
    if (!started_) {
      return;
    }
    playing_ = state == MediaPlaybackState::kPlaying;
    switch (state) {
      case MediaPlaybackState::kPlaying:
        MPNowPlayingInfoCenter.defaultCenter.playbackState = MPNowPlayingPlaybackStatePlaying;
        break;
      case MediaPlaybackState::kPaused:
        MPNowPlayingInfoCenter.defaultCenter.playbackState = MPNowPlayingPlaybackStatePaused;
        break;
      case MediaPlaybackState::kStopped:
        MPNowPlayingInfoCenter.defaultCenter.playbackState = MPNowPlayingPlaybackStateStopped;
        break;
      case MediaPlaybackState::kClosed:
        MPNowPlayingInfoCenter.defaultCenter.playbackState = MPNowPlayingPlaybackStateUnknown;
        break;
    }
    Republish();
  }

  void SetTimeline(const MediaTimeline& timeline) override {
    if (!started_) {
      return;
    }
    position_ms_ = timeline.position_ms;
    duration_ms_ = timeline.duration_ms;
    Republish();
  }

  [[nodiscard]] std::string description() const override {
    return started_ ? "MPNowPlayingInfoCenter (never tested on hardware)" : "not started";
  }

 private:
  // On this platform there is one dictionary and it holds everything, so a
  // timeline update is a read-modify-write of what is already published rather
  // than a call of its own.
  void Republish() {
    NSMutableDictionary* info =
        [MPNowPlayingInfoCenter.defaultCenter.nowPlayingInfo mutableCopy];
    if (info == nil) {
      return;
    }
    info[MPMediaItemPropertyPlaybackDuration] = @(duration_ms_ / 1000.0);
    info[MPNowPlayingInfoPropertyElapsedPlaybackTime] = @(position_ms_ / 1000.0);
    info[MPNowPlayingInfoPropertyPlaybackRate] = @(playing_ ? 1.0 : 0.0);
    MPNowPlayingInfoCenter.defaultCenter.nowPlayingInfo = info;
  }

  bool started_ = false;
  bool playing_ = false;
  std::int64_t position_ms_ = 0;
  std::int64_t duration_ms_ = 0;
  std::shared_ptr<Shared> shared_;
};

}  // namespace

std::unique_ptr<MediaIntegration> CreateMediaIntegration() {
  return std::make_unique<MacMediaIntegration>();
}

}  // namespace sonora::platform
