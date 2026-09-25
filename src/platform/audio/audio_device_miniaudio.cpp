#include <sonora/platform/audio_device.h>

#include <string>

#include "miniaudio.h"

// The one file in src/platform/ that is not split per operating system.
//
// ADR 0002 says the platform layer has one interface and one implementation
// directory per OS. This is the exception, and it is worth saying why rather
// than quietly breaking the rule: the per-OS part is real -- WASAPI on
// Windows, CoreAudio on macOS, ALSA or PulseAudio on Linux -- but it is
// miniaudio that implements it, not this project. Adding three files here that
// each call the same library with the same arguments would be ceremony, not
// abstraction.
//
// The rule still holds in the sense that matters: there is no #ifdef in this
// file, and nothing above src/platform/ knows which backend is running.
//
// What was given up by not writing WASAPI directly: exclusive mode, and the
// last few milliseconds of latency it buys. Neither is worth anything to a
// music player, where the mixer's shared mode is exactly what you want -- the
// user expects other applications to keep making sound. If a later week needs
// bit-perfect output, this is where a real WASAPI backend goes, behind the
// same interface, and nothing above it changes.

namespace sonora::platform {
namespace {

class MiniaudioDevice final : public AudioDevice {
 public:
  ~MiniaudioDevice() override { Stop(); }

  bool Start(AudioDeviceFormat requested, AudioRenderFn render, void* user_data) override {
    if (started_ || render == nullptr) {
      return false;
    }

    render_ = render;
    user_data_ = user_data;

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = static_cast<ma_uint32>(requested.channels);
    config.sampleRate = static_cast<ma_uint32>(requested.sample_rate_hz);
    config.dataCallback = &MiniaudioDevice::DataCallback;
    config.pUserData = this;

    // Shared mode, and the default period: a music player is a guest on the
    // machine's audio device, not the owner of it.
    config.playback.shareMode = ma_share_mode_shared;

    // Asks Windows to register the audio thread with MMCSS under "Pro Audio",
    // which is what keeps it scheduled ahead of ordinary work when the machine
    // is loaded. Ignored by every other backend, so it costs no conditional.
    config.wasapi.usage = ma_wasapi_usage_pro_audio;

    // miniaudio silences the output buffer before every callback. Render()
    // fills all of it unconditionally, so that is a memset of the whole buffer
    // done twice; turning it off means an unfilled buffer would play whatever
    // was in the memory, which is a far worse failure than a wasted memset.
    // Left on deliberately.

    if (ma_device_init(nullptr, &config, &device_) != MA_SUCCESS) {
      render_ = nullptr;
      return false;
    }
    initialised_ = true;

    if (ma_device_start(&device_) != MA_SUCCESS) {
      ma_device_uninit(&device_);
      initialised_ = false;
      render_ = nullptr;
      return false;
    }
    started_ = true;
    return true;
  }

  void Stop() override {
    if (initialised_) {
      // ma_device_uninit stops the device and joins its thread, so once this
      // returns the callback cannot run again and the engine it borrows is
      // safe to destroy.
      ma_device_uninit(&device_);
      initialised_ = false;
    }
    started_ = false;
    render_ = nullptr;
  }

  [[nodiscard]] AudioDeviceFormat format() const override {
    if (!initialised_) {
      return {};
    }
    return AudioDeviceFormat{static_cast<int>(device_.sampleRate),
                             static_cast<int>(device_.playback.channels)};
  }

  [[nodiscard]] std::size_t buffer_frames() const override {
    return initialised_ ? device_.playback.internalPeriodSizeInFrames : 0;
  }

  [[nodiscard]] std::string description() const override {
    if (!initialised_) {
      return "no device";
    }
    std::string name = device_.playback.name;
    const char* backend = ma_get_backend_name(device_.pContext->backend);
    return name.empty() ? std::string(backend) : name + " (" + backend + ")";
  }

 private:
  // The real-time entry point. Everything it is allowed to do is decided by
  // ADR 0006, and all it does here is forward -- deliberately, so that the
  // rule has one place to be broken and one place to look.
  static void DataCallback(ma_device* device,
                           void* output,
                           const void* input,
                           ma_uint32 frame_count) {
    (void)input;  // playback only
    auto* self = static_cast<MiniaudioDevice*>(device->pUserData);
    self->render_(static_cast<float*>(output), static_cast<std::size_t>(frame_count),
                  self->user_data_);
  }

  ma_device device_{};
  bool initialised_ = false;
  bool started_ = false;
  AudioRenderFn render_ = nullptr;
  void* user_data_ = nullptr;
};

}  // namespace

std::unique_ptr<AudioDevice> CreateAudioDevice() {
  return std::make_unique<MiniaudioDevice>();
}

}  // namespace sonora::platform
