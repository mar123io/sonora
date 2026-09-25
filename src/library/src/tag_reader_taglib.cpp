#include <sonora/library/tag_reader.h>

#include <taglib/attachedpictureframe.h>
#include <taglib/audioproperties.h>
#include <taglib/fileref.h>
#include <taglib/flacfile.h>
#include <taglib/flacpicture.h>
#include <taglib/id3v2tag.h>
#include <taglib/mpegfile.h>
#include <taglib/tag.h>
#include <taglib/tpropertymap.h>
#include <taglib/tstring.h>
#include <taglib/wavfile.h>

#include <cstdlib>
#include <string>
#include <vector>

namespace sonora::library {
namespace {

// The only file in the project that includes a TagLib header. Everything above
// it sees the TagReader interface, which is what keeps a scan testable without
// a real FLAC and what keeps the dependency out of every other target.

[[nodiscard]] std::string ToUtf8(const TagLib::String& text) {
  // to8Bit(true) means UTF-8. to8Bit(false) means Latin-1 and silently mangles
  // every accented name in the library, which is most of an Italian one.
  return text.isEmpty() ? std::string() : text.to8Bit(true);
}

// Tags write these as "3", as "3/12", and occasionally with spaces around the
// slash. Only the part before the slash is the number; the rest is how many
// there are in total, which the index does not store.
[[nodiscard]] int LeadingNumber(const std::string& text) {
  std::size_t index = 0;
  while (index < text.size() && (text[index] == ' ' || text[index] == '\t')) {
    ++index;
  }
  int value = 0;
  bool any = false;
  while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
    // A tag is not trusted input. Clamp rather than overflow: a disc number of
    // two billion is a broken tag, not a disc.
    if (value < 100000) {
      value = value * 10 + (text[index] - '0');
    }
    any = true;
    ++index;
  }
  return any ? value : 0;
}

[[nodiscard]] std::string FirstProperty(const TagLib::PropertyMap& properties,
                                        const char* key) {
  // The property map is TagLib's one vocabulary across ID3v2, Vorbis comments
  // and MP4 atoms. Reaching for the format-specific classes instead would mean
  // a dynamic_cast per format and three code paths for "album artist".
  //
  // find() rather than operator[]: the const operator[] of a TagLib map, asked
  // for a key that is not there, writes a line to TagLib's debug log and hands
  // back a shared dummy. Most files have no ALBUMARTIST, so that is the normal
  // case, not the exceptional one. Keys are uppercase by TagLib's convention.
  const TagLib::PropertyMap::ConstIterator entry = properties.find(key);
  if (entry == properties.end() || entry->second.isEmpty()) {
    return {};
  }
  return ToUtf8(entry->second.front());
}

// A cover of more than this is a broken tag, not a cover. Album art is a few
// hundred kilobytes; somebody's 20 MB scan of a gatefold sleeve would be read,
// hashed and stored on every scan for a picture that will be drawn 48 pixels
// wide.
constexpr std::size_t kMaxCoverBytes = 8u * 1024u * 1024u;

// What these bytes actually are, whatever the tag says they are.
//
// The tag's declared mime type is written by whichever program last touched the
// file, and the response that serves this picture carries
// X-Content-Type-Options: nosniff -- so a wrong type is a broken image and no
// message. The magic numbers are unambiguous and four bytes long; reading them
// is cheaper than trusting a string.
[[nodiscard]] std::string SniffImageMime(const std::vector<std::uint8_t>& bytes) {
  const auto starts_with = [&bytes](std::initializer_list<std::uint8_t> magic) {
    if (bytes.size() < magic.size()) {
      return false;
    }
    std::size_t index = 0;
    for (const std::uint8_t byte : magic) {
      if (bytes[index++] != byte) {
        return false;
      }
    }
    return true;
  };

  if (starts_with({0xFF, 0xD8, 0xFF})) {
    return "image/jpeg";
  }
  if (starts_with({0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A})) {
    return "image/png";
  }
  if (starts_with({'G', 'I', 'F', '8'})) {
    return "image/gif";
  }
  if (starts_with({'R', 'I', 'F', 'F'}) && bytes.size() >= 12 && bytes[8] == 'W' &&
      bytes[9] == 'E' && bytes[10] == 'B' && bytes[11] == 'P') {
    return "image/webp";
  }
  if (starts_with({'B', 'M'})) {
    return "image/bmp";
  }
  // Anything else is not something a browser will draw, so it is not a cover.
  return {};
}

[[nodiscard]] std::optional<Cover> MakeCover(const TagLib::ByteVector& data) {
  if (data.isEmpty() || data.size() > kMaxCoverBytes) {
    return std::nullopt;
  }

  Cover cover;
  const auto* begin = reinterpret_cast<const std::uint8_t*>(data.data());
  cover.bytes.assign(begin, begin + data.size());
  cover.mime = SniffImageMime(cover.bytes);
  if (cover.mime.empty()) {
    return std::nullopt;
  }
  return cover;
}

// ID3v2 keeps pictures in APIC frames, one per picture, each declaring what it
// is a picture of. The front cover is what a track list wants; a file that
// declares no cover at all gets its first picture rather than nothing, because
// plenty of taggers leave the type as Other.
[[nodiscard]] std::optional<Cover> FirstPicture(const TagLib::ID3v2::Tag* tag) {
  const TagLib::ID3v2::FrameList& frames = tag->frameList("APIC");
  const TagLib::ID3v2::AttachedPictureFrame* fallback = nullptr;

  for (const TagLib::ID3v2::Frame* frame : frames) {
    const auto* picture = dynamic_cast<const TagLib::ID3v2::AttachedPictureFrame*>(frame);
    if (picture == nullptr) {
      continue;
    }
    if (picture->type() == TagLib::ID3v2::AttachedPictureFrame::FrontCover) {
      return MakeCover(picture->picture());
    }
    if (fallback == nullptr) {
      fallback = picture;
    }
  }
  return fallback != nullptr ? MakeCover(fallback->picture()) : std::nullopt;
}

// The front cover, per container format.
//
// TagLib 2 has complexProperties(), which would collapse all of this into one
// lookup of "PICTURE" -- and would also pin this project to TagLib 2, which is
// what vcpkg happens to install today and not what every system package has.
// The three cases below are the formats src/library indexes; another format
// means another case, which is the honest cost of supporting both.
[[nodiscard]] std::optional<Cover> ReadCover(TagLib::File* file) {
  if (auto* mpeg = dynamic_cast<TagLib::MPEG::File*>(file); mpeg != nullptr) {
    // Dispatched on the type FileRef chose from the file's content, not on the
    // extension -- same reason as src/audio/src/codecs.cpp.
    const TagLib::ID3v2::Tag* tag = mpeg->ID3v2Tag();
    if (tag == nullptr) {
      return std::nullopt;
    }
    return FirstPicture(tag);
  }
  if (auto* flac = dynamic_cast<TagLib::FLAC::File*>(file); flac != nullptr) {
    // A FLAC can carry several pictures: a front cover, a back cover, a picture
    // of the band. The front cover is the one a list wants, and a file that
    // declares none gets its first picture rather than nothing.
    const TagLib::List<TagLib::FLAC::Picture*> pictures = flac->pictureList();
    const TagLib::FLAC::Picture* fallback = nullptr;
    for (const TagLib::FLAC::Picture* picture : pictures) {
      if (picture == nullptr) {
        continue;
      }
      if (picture->type() == TagLib::FLAC::Picture::FrontCover) {
        return MakeCover(picture->data());
      }
      if (fallback == nullptr) {
        fallback = picture;
      }
    }
    return fallback != nullptr ? MakeCover(fallback->data()) : std::nullopt;
  }
  if (auto* wav = dynamic_cast<TagLib::RIFF::WAV::File*>(file); wav != nullptr) {
    const TagLib::ID3v2::Tag* tag = wav->ID3v2Tag();
    return tag != nullptr ? FirstPicture(tag) : std::nullopt;
  }
  return std::nullopt;
}

class TagLibReader final : public TagReader {
 public:
  std::optional<TagData> Read(const std::filesystem::path& path,
                              bool with_cover) const override {
    // path::c_str() is wchar_t* on Windows and char* elsewhere, and
    // TagLib::FileName has a constructor for each. That is the whole reason
    // there is no #ifdef here, and the reason a track in D:\Müsik\… opens.
    const TagLib::FileRef file(path.c_str(), true, TagLib::AudioProperties::Average);
    if (file.isNull() || file.file() == nullptr) {
      return std::nullopt;
    }

    TagData tags;
    if (const TagLib::Tag* tag = file.tag(); tag != nullptr) {
      tags.title = ToUtf8(tag->title());
      tags.artist = ToUtf8(tag->artist());
      tags.album = ToUtf8(tag->album());
      tags.track_number = static_cast<int>(tag->track());
      tags.year = static_cast<int>(tag->year());
    }

    const TagLib::PropertyMap properties = file.file()->properties();
    tags.album_artist = FirstProperty(properties, "ALBUMARTIST");
    tags.disc_number = LeadingNumber(FirstProperty(properties, "DISCNUMBER"));

    if (const TagLib::AudioProperties* audio = file.audioProperties(); audio != nullptr) {
      tags.duration_ms = audio->lengthInMilliseconds();
    }

    if (with_cover) {
      tags.cover = ReadCover(file.file());
    }

    // A file with no tags at all still belongs in the library: it has a name, a
    // length and a path, and the scanner fills the title in from the file name.
    // Returning nullopt here would hide it instead.
    return tags;
  }

  [[nodiscard]] std::string description() const override { return "TagLib"; }
};

}  // namespace

TagReaderPtr MakeTagLibReader() {
  return std::make_unique<TagLibReader>();
}

}  // namespace sonora::library
