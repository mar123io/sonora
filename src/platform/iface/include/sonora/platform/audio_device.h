#pragma once

#include <cstddef>
#include <memory>
#include <string>

namespace sonora::platform {

struct AudioDeviceFormat {
  int sample_rate_hz = 0;
  int channels = 0;
};

// Called on the operating system's audio thread, which belongs to the
// operating system and not to this program.
//
// REAL TIME. No allocation, no locks, no I/O, no logging, no exceptions. The
// rule and the reasoning are in
// docs/adr/0006-the-audio-callback-is-real-time.md.
//
// A raw function pointer and a void*, not a std::function: a std::function may
// allocate when it is assigned and costs an indirection when it is called, and
// neither is something to spend on the one path in this program with a
// deadline. It also makes the calling convention obvious at the boundary,
// which matters when the caller is a C library.
using AudioRenderFn = void (*)(float* output, std::size_t frames, void* user_data);

// A playback device, open at one format.
//
// Nothing here resamples or mixes. The engine above decides what the samples
// are; this only moves them to the hardware and reports what the hardware
// actually agreed to.
class AudioDevice {
 public:
  virtual ~AudioDevice() = default;

  AudioDevice(const AudioDevice&) = delete;
  AudioDevice& operator=(const AudioDevice&) = delete;

  // `requested` is a request. The device may open at a different rate or
  // channel count and convert; format() reports what the callback will
  // actually be handed, and that is the one to trust.
  virtual bool Start(AudioDeviceFormat requested, AudioRenderFn render, void* user_data) = 0;

  // Returns once the callback is guaranteed not to run again, so whatever it
  // borrows can be destroyed afterwards. Calling it twice is fine.
  virtual void Stop() = 0;

  [[nodiscard]] virtual AudioDeviceFormat format() const = 0;

  // Frames per callback, as the backend has it. Useful for sizing buffers and
  // for saying something honest in a log line about latency.
  [[nodiscard]] virtual std::size_t buffer_frames() const = 0;

  // Backend and device name, for the startup line. Free-form, for people.
  [[nodiscard]] virtual std::string description() const = 0;

 protected:
  AudioDevice() = default;
};

// Never null. A platform with no implementation returns a device whose Start()
// fails with a message, rather than a null pointer every caller has to check.
[[nodiscard]] std::unique_ptr<AudioDevice> CreateAudioDevice();

}  // namespace sonora::platform
