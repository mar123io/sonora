#include <sonora/audio/codecs.h>

#include <fstream>
#include <memory>
#include <utility>

#include "miniaudio.h"

namespace sonora::audio {
namespace {

// Why a stream and its callbacks rather than ma_decoder_init_file():
//
// ma_decoder_init_file() takes a const char*, which on Windows means the
// system's narrow code page. A path with a character that code page cannot
// represent fails to open, and half the music libraries in the world contain
// one. The wide variant exists, and using it would put a #ifdef _WIN32 in this
// file, which ADR 0002 forbids outside src/platform.
//
// std::ifstream takes a std::filesystem::path and does the right thing on
// every platform, so the way out is to hand miniaudio a stream rather than a
// name. It is also the shape a network source will need later, which is a
// reason to prefer it even where the narrow path would have worked.
class FileSource {
 public:
  explicit FileSource(const std::filesystem::path& path)
      : stream_(path, std::ios::binary | std::ios::in) {}

  [[nodiscard]] bool is_open() const { return stream_.is_open(); }

  static ma_result OnRead(ma_decoder* decoder,
                          void* buffer,
                          std::size_t bytes,
                          std::size_t* read) {
    auto* self = static_cast<FileSource*>(decoder->pUserData);
    self->stream_.read(static_cast<char*>(buffer), static_cast<std::streamsize>(bytes));
    const auto count = static_cast<std::size_t>(self->stream_.gcount());
    *read = count;

    // eof after a partial read is not a failure: the caller asked for more
    // than was left. Only clear it so the next seek can still work.
    if (self->stream_.eof()) {
      self->stream_.clear();
    }
    if (count == 0) {
      return MA_AT_END;
    }
    return MA_SUCCESS;
  }

  static ma_result OnSeek(ma_decoder* decoder, ma_int64 offset, ma_seek_origin origin) {
    auto* self = static_cast<FileSource*>(decoder->pUserData);
    self->stream_.clear();
    self->stream_.seekg(static_cast<std::streamoff>(offset),
                        origin == ma_seek_origin_current ? std::ios::cur : std::ios::beg);
    return self->stream_.fail() ? MA_ERROR : MA_SUCCESS;
  }

 private:
  std::ifstream stream_;
};

// A decoder that owns both the file and miniaudio's state, so closing one
// cannot outlive the other.
class MiniaudioDecoder final : public Decoder {
 public:
  MiniaudioDecoder(std::unique_ptr<FileSource> source, const std::filesystem::path& path)
      : source_(std::move(source)) {
    const ma_decoder_config config =
        // 0 channels and 0 sample rate mean "whatever the file is". Converting
        // here would hide from the device the one fact it needs in order to
        // open at the source's own rate, which is how week 5 avoids owning a
        // resampler.
        ma_decoder_config_init(ma_format_f32, 0, 0);

    const ma_result result = ma_decoder_init(&FileSource::OnRead, &FileSource::OnSeek,
                                             source_.get(), &config, &decoder_);
    if (result != MA_SUCCESS) {
      throw DecoderError("cannot decode '" + path.string() +
                         "': " + ma_result_description(result) +
                         " (supported: " + SupportedFormats() + ")");
    }
    initialised_ = true;

    ma_format format = ma_format_unknown;
    ma_uint32 channels = 0;
    ma_uint32 sample_rate = 0;
    ma_decoder_get_data_format(&decoder_, &format, &channels, &sample_rate, nullptr, 0);
    format_.channels = static_cast<int>(channels);
    format_.sample_rate_hz = static_cast<int>(sample_rate);

    // Absent for a stream and for some MP3s, in which case the length stays 0
    // and everything downstream has to cope. Reporting a wrong number would be
    // worse than reporting none.
    ma_uint64 length = 0;
    if (ma_decoder_get_length_in_pcm_frames(&decoder_, &length) == MA_SUCCESS) {
      total_frames_ = length;
    }
  }

  ~MiniaudioDecoder() override {
    if (initialised_) {
      ma_decoder_uninit(&decoder_);
    }
  }

  MiniaudioDecoder(const MiniaudioDecoder&) = delete;
  MiniaudioDecoder& operator=(const MiniaudioDecoder&) = delete;

  [[nodiscard]] AudioFormat format() const noexcept override { return format_; }
  [[nodiscard]] std::uint64_t total_frames() const noexcept override { return total_frames_; }

  std::size_t Read(float* output, std::size_t frames) override {
    ma_uint64 read = 0;
    // MA_AT_END comes with read == 0. Anything else non-success is a broken
    // file, and it is reported the same way: no more samples. The engine above
    // cannot do anything different about the two, and pretending otherwise
    // would put an error path in the decode thread that nobody handles.
    ma_decoder_read_pcm_frames(&decoder_, output, frames, &read);
    return static_cast<std::size_t>(read);
  }

  bool Seek(std::uint64_t frame) override {
    return ma_decoder_seek_to_pcm_frame(&decoder_, frame) == MA_SUCCESS;
  }

 private:
  std::unique_ptr<FileSource> source_;
  ma_decoder decoder_{};
  bool initialised_ = false;
  AudioFormat format_{};
  std::uint64_t total_frames_ = 0;
};

}  // namespace

DecoderPtr OpenFileDecoder(const std::filesystem::path& path) {
  auto source = std::make_unique<FileSource>(path);
  if (!source->is_open()) {
    throw DecoderError("cannot open '" + path.string() + "'");
  }
  return std::make_unique<MiniaudioDecoder>(std::move(source), path);
}

std::string SupportedFormats() {
  // Vorbis is missing on purpose. miniaudio carries WAV, FLAC and MP3 with it;
  // Ogg needs stb_vorbis dropped in alongside, which is a second decoder with
  // its own memory behaviour to review. The Decoder interface is what makes
  // that a new file rather than a change to this one, so it can wait until
  // something actually needs it.
  return "wav, flac, mp3";
}

}  // namespace sonora::audio
