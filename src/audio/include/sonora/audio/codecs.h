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
// `target` asks the decoder to convert as it goes: channels and sample rate,
// done by the same library that does the decoding. Leave it default-constructed
// for the file's own format.
//
// This is what makes gapless possible at all. A queue whose tracks are 44.1 kHz
// and 48 kHz cannot be played through one device without somebody resampling,
// and reopening the device between two tracks is exactly the gap week 6 exists
// to remove. Converting at the decoder means every track arrives in the format
// the device already has.
[[nodiscard]] DecoderPtr OpenFileDecoder(const std::filesystem::path& path,
                                         AudioFormat target = {});

// What this build can open, for the startup banner and for the error message
// when it cannot open something.
[[nodiscard]] std::string SupportedFormats();

}  // namespace sonora::audio
