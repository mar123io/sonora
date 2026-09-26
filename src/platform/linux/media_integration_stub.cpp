#include <sonora/platform/media_integration.h>

#include <memory>

namespace sonora::platform {
namespace {

// Linux has an answer to this and it is MPRIS: a D-Bus interface
// (org.mpris.MediaPlayer2.Player) that every desktop environment's panel,
// lock screen and keyboard shortcut daemon already speaks. It is a genuine
// implementation -- an object exported on the session bus, properties,
// PropertiesChanged signals -- and it is a dependency (sdbus or GDBus) plus a
// day, on a platform this project does not yet support at all.
//
// So it says no, clearly, once. The rest of the shell treats "no media
// integration" as a normal condition already, because a Windows N edition or a
// managed policy can produce the same answer on the platform that is supported.
class StubMediaIntegration final : public MediaIntegration {
 public:
  bool Start(void*, MediaRequestFn) override { return false; }
  void Stop() override {}
  void SetMetadata(const MediaMetadata&) override {}
  void SetPlaybackState(MediaPlaybackState) override {}
  void SetTimeline(const MediaTimeline&) override {}

  [[nodiscard]] std::string description() const override {
    return "unavailable: Linux needs an MPRIS implementation (not written)";
  }
};

}  // namespace

std::unique_ptr<MediaIntegration> CreateMediaIntegration() {
  return std::make_unique<StubMediaIntegration>();
}

}  // namespace sonora::platform
