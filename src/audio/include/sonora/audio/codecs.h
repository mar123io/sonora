#pragma once

#include <filesystem>
#include <string>

#include <sonora/audio/decoder.h>

namespace sonora::audio {

// Opens an audio file and returns a decoder for it, or throws DecoderError
// with a message meant to be shown to a person.
//
// The format is detected from the content, not from the extension. A .mp3 that
// is really a FLAC is a thing that exists in every music library that has ever
// been converted by a script, and refusing it on the strength of four
// characters of file name is a bad reason to refuse it.
[[nodiscard]] DecoderPtr OpenFileDecoder(const std::filesystem::path& path);

// What this build can open, for the startup banner and for the error message
// when it cannot open something.
[[nodiscard]] std::string SupportedFormats();

}  // namespace sonora::audio
