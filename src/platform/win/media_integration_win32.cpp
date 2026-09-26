#include <sonora/platform/media_integration.h>

#include <windows.h>

#include <systemmediatransportcontrolsinterop.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Storage.Streams.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <utility>

namespace sonora::platform {
namespace {

namespace media = winrt::Windows::Media;
namespace streams = winrt::Windows::Storage::Streams;

using namespace std::chrono_literals;

[[nodiscard]] winrt::hstring ToHstring(const std::string& utf8) {
  return winrt::to_hstring(utf8);
}

[[nodiscard]] winrt::Windows::Foundation::TimeSpan Milliseconds(std::int64_t value) {
  return std::chrono::duration_cast<winrt::Windows::Foundation::TimeSpan>(
      std::chrono::milliseconds(value < 0 ? 0 : value));
}

// The callback, and the fact that it outlives us.
//
// The system raises button events on its own thread pool, and a handler
// registered with SystemMediaTransportControls can fire after this object has
// gone -- a media key pressed while the window is closing is not an exotic
// case, it is Tuesday. So the handlers capture this by value instead of
// capturing `this`, and Stop() empties it: a late event then finds nothing to
// call rather than a destroyed object.
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

// The artwork, on a background thread, because every step of putting bytes into
// a WinRT stream is asynchronous.
//
// Blocking on those with .get() from the thread that owns the window is the
// classic way to deadlock an STA, and C++/WinRT refuses to let you: get() on a
// UI thread throws. So the work moves off the thread and comes back when it is
// done.
//
// It captures nothing but values -- the updater is a WinRT object with its own
// lifetime, the bytes are copied, and the generation is shared -- so it cannot
// outlive anything it points at. The generation is what stops two quick track
// changes from racing: the older coroutine notices it has been overtaken and
// drops its picture instead of painting it over the newer one.
winrt::fire_and_forget ApplyThumbnail(media::SystemMediaTransportControlsDisplayUpdater updater,
                                      std::vector<std::uint8_t> bytes,
                                      std::shared_ptr<std::atomic<std::uint64_t>> generation,
                                      std::uint64_t mine) {
  co_await winrt::resume_background();

  try {
    streams::InMemoryRandomAccessStream stream;
    streams::DataWriter writer(stream.GetOutputStreamAt(0));
    writer.WriteBytes(
        winrt::array_view<const std::uint8_t>(bytes.data(), bytes.data() + bytes.size()));
    co_await writer.StoreAsync();
    co_await writer.FlushAsync();
    writer.DetachStream();
    stream.Seek(0);

    if (generation->load() != mine) {
      co_return;  // overtaken while the bytes were being written
    }
    updater.Thumbnail(streams::RandomAccessStreamReference::CreateFromStream(stream));
    updater.Update();
  } catch (const winrt::hresult_error&) {
    // A cover that cannot be handed over is a cover the panel does not show.
    // Nothing else about playback depends on it.
  }
}

class WindowsMediaIntegration final : public MediaIntegration {
 public:
  ~WindowsMediaIntegration() override { Stop(); }

  bool Start(void* native_window, MediaRequestFn on_request) override {
    if (native_window == nullptr) {
      return false;
    }

    try {
      // The thread already belongs to somebody else. CEF initialises COM on the
      // UI thread before this runs, so asking for an apartment here is asking a
      // question that has been answered: RO_E_CHANGED_MODE means "it is already
      // something else", which is fine, and is why this is not an error.
      //
      // For the same reason nothing here calls uninit_apartment: this code did
      // not open the apartment and does not get to close it.
      try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
      } catch (const winrt::hresult_error& error) {
        if (error.code() != RPC_E_CHANGED_MODE) {
          throw;
        }
      }

      // A Win32 window has no CoreWindow, so the usual GetForCurrentView does
      // not apply. The interop interface is how a desktop application asks for
      // the session that belongs to its own window -- and tying it to a window
      // is also what makes the system show this application's name and icon.
      const auto interop =
          winrt::get_activation_factory<media::SystemMediaTransportControls,
                                        ISystemMediaTransportControlsInterop>();
      winrt::check_hresult(interop->GetForWindow(
          static_cast<HWND>(native_window),
          winrt::guid_of<media::SystemMediaTransportControls>(), winrt::put_abi(controls_)));

      shared_ = std::make_shared<Shared>();
      {
        const std::lock_guard<std::mutex> lock(shared_->mutex);
        shared_->on_request = std::move(on_request);
      }

      controls_.IsPlayEnabled(true);
      controls_.IsPauseEnabled(true);
      controls_.IsStopEnabled(true);
      controls_.IsNextEnabled(true);
      controls_.IsPreviousEnabled(true);
      controls_.PlaybackStatus(media::MediaPlaybackStatus::Closed);
      controls_.IsEnabled(true);

      auto shared = shared_;
      button_token_ = controls_.ButtonPressed(
          [shared](const media::SystemMediaTransportControls&,
                   const media::SystemMediaTransportControlsButtonPressedEventArgs& args) {
            switch (args.Button()) {
              case media::SystemMediaTransportControlsButton::Play:
                shared->Dispatch(MediaCommand::kPlay);
                break;
              case media::SystemMediaTransportControlsButton::Pause:
                shared->Dispatch(MediaCommand::kPause);
                break;
              case media::SystemMediaTransportControlsButton::Stop:
                shared->Dispatch(MediaCommand::kStop);
                break;
              case media::SystemMediaTransportControlsButton::Next:
                shared->Dispatch(MediaCommand::kNext);
                break;
              case media::SystemMediaTransportControlsButton::Previous:
                shared->Dispatch(MediaCommand::kPrevious);
                break;
              default:
                // Record, ChannelUp and the rest belong to other kinds of
                // application. Ignored, not refused.
                break;
            }
          });

      // Dragging the panel's progress bar. Separate from the buttons because it
      // carries a value, and available only because the timeline was published:
      // a session that never calls UpdateTimelineProperties gets no scrub bar
      // and this event never fires.
      position_token_ = controls_.PlaybackPositionChangeRequested(
          [shared](const media::SystemMediaTransportControls&,
                   const media::PlaybackPositionChangeRequestedEventArgs& args) {
            const auto position = std::chrono::duration_cast<std::chrono::milliseconds>(
                args.RequestedPlaybackPosition());
            shared->Dispatch(MediaCommand::kSeek, position.count());
          });

      return true;
    } catch (const winrt::hresult_error& error) {
      // Windows N editions, a locked-down policy, a window that is not eligible:
      // all of them end here, and none of them should stop the music.
      last_error_ = winrt::to_string(error.message());
      controls_ = nullptr;
      shared_.reset();
      return false;
    }
  }

  void Stop() override {
    if (shared_) {
      // First, so a media key pressed during teardown finds an empty handler
      // rather than a half-destroyed program.
      const std::lock_guard<std::mutex> lock(shared_->mutex);
      shared_->on_request = nullptr;
    }
    if (!controls_) {
      return;
    }

    try {
      controls_.ButtonPressed(button_token_);
      controls_.PlaybackPositionChangeRequested(position_token_);
      controls_.PlaybackStatus(media::MediaPlaybackStatus::Closed);
      controls_.IsEnabled(false);

      auto updater = controls_.DisplayUpdater();
      updater.ClearAll();
      updater.Update();
    } catch (const winrt::hresult_error&) {
      // Tearing down a session that the system has already torn down.
    }
    controls_ = nullptr;
    shared_.reset();
  }

  void SetMetadata(const MediaMetadata& metadata) override {
    if (!controls_) {
      return;
    }
    try {
      auto updater = controls_.DisplayUpdater();
      updater.Type(media::MediaPlaybackType::Music);

      auto music = updater.MusicProperties();
      music.Title(ToHstring(metadata.title));
      music.Artist(ToHstring(metadata.artist));
      music.AlbumTitle(ToHstring(metadata.album));
      if (metadata.track_number > 0) {
        music.TrackNumber(static_cast<std::uint32_t>(metadata.track_number));
      }

      const std::uint64_t mine = generation_->fetch_add(1) + 1;
      if (metadata.art.empty()) {
        updater.Thumbnail(nullptr);
        updater.Update();
        return;
      }

      // Updated twice on purpose: once now, so the title is right immediately,
      // and once when the picture is ready. A moment of the previous cover under
      // the correct title is a better wrong than a moment of no title at all.
      updater.Update();
      ApplyThumbnail(updater, metadata.art, generation_, mine);
    } catch (const winrt::hresult_error&) {
    }
  }

  void SetPlaybackState(MediaPlaybackState state) override {
    if (!controls_) {
      return;
    }
    try {
      controls_.PlaybackStatus([state] {
        switch (state) {
          case MediaPlaybackState::kPlaying:
            return media::MediaPlaybackStatus::Playing;
          case MediaPlaybackState::kPaused:
            return media::MediaPlaybackStatus::Paused;
          case MediaPlaybackState::kStopped:
            return media::MediaPlaybackStatus::Stopped;
          case MediaPlaybackState::kClosed:
            break;
        }
        return media::MediaPlaybackStatus::Closed;
      }());
    } catch (const winrt::hresult_error&) {
    }
  }

  void SetTimeline(const MediaTimeline& timeline) override {
    if (!controls_) {
      return;
    }
    try {
      media::SystemMediaTransportControlsTimelineProperties properties;
      properties.StartTime(Milliseconds(0));
      properties.MinSeekTime(Milliseconds(0));
      properties.Position(Milliseconds(timeline.position_ms));
      properties.MaxSeekTime(Milliseconds(timeline.duration_ms));
      properties.EndTime(Milliseconds(timeline.duration_ms));
      controls_.UpdateTimelineProperties(properties);
    } catch (const winrt::hresult_error&) {
    }
  }

  [[nodiscard]] std::string description() const override {
    if (controls_) {
      return "SystemMediaTransportControls";
    }
    return last_error_.empty() ? "not started" : "unavailable: " + last_error_;
  }

 private:
  media::SystemMediaTransportControls controls_{nullptr};
  winrt::event_token button_token_{};
  winrt::event_token position_token_{};
  std::shared_ptr<Shared> shared_;
  std::shared_ptr<std::atomic<std::uint64_t>> generation_ =
      std::make_shared<std::atomic<std::uint64_t>>(0);
  std::string last_error_;
};

}  // namespace

std::unique_ptr<MediaIntegration> CreateMediaIntegration() {
  return std::make_unique<WindowsMediaIntegration>();
}

}  // namespace sonora::platform
