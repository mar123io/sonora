#include "sonora/update/manifest.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>

namespace sonora::update {
namespace {

using Json = nlohmann::json;

// https only, no userinfo, no control characters, no whitespace.
//
// The scheme is a rule rather than a preference: an http url in a signed manifest
// would still be authenticated by the hash next to it, but it would also be a
// plaintext request announcing which version this machine is running, to anybody
// on the path.
//
// The userinfo check is the one that looks paranoid and is not.
// "https://downloads.sonora.app@evil.example/x" has host evil.example and reads,
// to a person reviewing a manifest, as the first name in it.
bool IsAcceptableUrl(std::string_view url) {
  constexpr std::string_view kScheme = "https://";
  if (url.size() < kScheme.size() + 1 || url.size() > kMaxUrlLength) {
    return false;
  }
  if (url.substr(0, kScheme.size()) != kScheme) {
    return false;
  }
  for (const char c : url) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte <= 0x20 || byte == 0x7F) {
      return false;
    }
  }
  const std::string_view rest = url.substr(kScheme.size());
  const std::size_t slash = rest.find('/');
  std::string_view authority = slash == std::string_view::npos ? rest : rest.substr(0, slash);
  if (authority.empty() || authority.find('@') != std::string_view::npos) {
    return false;
  }

  // An optional port, and then a host that looks like a host: letters, digits,
  // dots and hyphens, in non-empty labels that do not begin or end with a hyphen.
  // Not a full RFC 3986 parser, and not trying to be -- the manifest is generated
  // by a workflow whose urls are three string concatenations, so anything else in
  // here is a bug in that workflow, and the point of the check is to find it in CI
  // rather than on a user's machine.
  if (const std::size_t colon = authority.find(':'); colon != std::string_view::npos) {
    const std::string_view port = authority.substr(colon + 1);
    if (port.empty() || port.size() > 5) {
      return false;
    }
    for (const char c : port) {
      if (c < '0' || c > '9') {
        return false;
      }
    }
    authority = authority.substr(0, colon);
  }
  if (authority.empty()) {
    return false;
  }
  std::size_t label_start = 0;
  while (label_start <= authority.size()) {
    const std::size_t dot = authority.find('.', label_start);
    const std::size_t end = dot == std::string_view::npos ? authority.size() : dot;
    const std::string_view label = authority.substr(label_start, end - label_start);
    if (label.empty() || label.front() == '-' || label.back() == '-') {
      return false;
    }
    for (const char c : label) {
      const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '-';
      if (!allowed) {
        return false;
      }
    }
    if (dot == std::string_view::npos) {
      break;
    }
    label_start = dot + 1;
  }
  return true;
}

bool IsAcceptablePlatform(std::string_view platform) {
  if (platform.empty() || platform.size() > 32) {
    return false;
  }
  for (const char c : platform) {
    const bool allowed = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    if (!allowed) {
      return false;
    }
  }
  return true;
}

bool ReadSize(const Json& node, std::uint64_t& out) {
  if (!node.is_number_unsigned()) {
    return false;
  }
  const auto value = node.get<std::uint64_t>();
  // Zero is not a size. An artefact of no bytes is a release that was published
  // before it was built.
  if (value == 0 || value > 16ull * 1024 * 1024 * 1024) {
    return false;
  }
  out = value;
  return true;
}

bool ReadHash(const Json& node, Hash256& out) {
  if (!node.is_string()) {
    return false;
  }
  const auto parsed = ParseHash(node.get_ref<const std::string&>());
  if (!parsed.has_value()) {
    return false;
  }
  out = *parsed;
  return true;
}

// `want_url` distinguishes the served artefacts from the uncompressed archive,
// which has no url on purpose.
bool ReadArtifact(const Json& node, bool want_url, Artifact& out, ManifestError& error) {
  if (!node.is_object()) {
    error = ManifestError::kBadArtifact;
    return false;
  }
  if (!node.contains("size") || !ReadSize(node["size"], out.size)) {
    error = ManifestError::kBadArtifact;
    return false;
  }
  if (!node.contains("hash") || !ReadHash(node["hash"], out.hash)) {
    error = ManifestError::kBadHash;
    return false;
  }
  if (want_url) {
    if (!node.contains("url") || !node["url"].is_string()) {
      error = ManifestError::kBadUrl;
      return false;
    }
    out.url = node["url"].get<std::string>();
    if (!IsAcceptableUrl(out.url)) {
      error = ManifestError::kBadUrl;
      return false;
    }
  } else if (node.contains("url")) {
    // Not "ignored": an url on the uncompressed archive means whoever generated
    // this manifest believes it is downloadable, and one of us is wrong.
    error = ManifestError::kBadArtifact;
    return false;
  }
  return true;
}

bool ReadVersion(const Json& node, Version& out) {
  if (!node.is_string()) {
    return false;
  }
  const auto parsed = ParseVersion(node.get_ref<const std::string&>());
  if (!parsed.has_value()) {
    return false;
  }
  out = *parsed;
  return true;
}

}  // namespace

std::string_view Describe(ManifestError error) {
  switch (error) {
    case ManifestError::kNone:
      return "no error";
    case ManifestError::kTooLarge:
      return "the manifest is larger than a manifest can be";
    case ManifestError::kNotJson:
      return "the manifest is not valid JSON";
    case ManifestError::kNotAnObject:
      return "the manifest is not a JSON object";
    case ManifestError::kUnsupportedSchema:
      return "a manifest schema this version does not understand";
    case ManifestError::kBadChannel:
      return "a missing or unusable channel name";
    case ManifestError::kBadReleases:
      return "the releases are missing or are not a list";
    case ManifestError::kBadVersion:
      return "a version that is not X.Y.Z";
    case ManifestError::kBadPlatform:
      return "a platform name that is not one";
    case ManifestError::kBadArtifact:
      return "an artefact without a usable size and hash";
    case ManifestError::kBadUrl:
      return "a url that is not an https url without credentials";
    case ManifestError::kBadHash:
      return "a hash that is not 64 hex digits";
    case ManifestError::kBadDelta:
      return "a delta that does not come from an older version, or is repeated";
    case ManifestError::kDuplicateRelease:
      return "the same version and platform twice";
    case ManifestError::kTooMany:
      return "more releases or deltas than this version will read";
  }
  return "unknown error";
}

std::optional<Manifest> ParseManifest(std::string_view json, ManifestError& error) {
  error = ManifestError::kNone;

  if (json.empty() || json.size() > kMaxManifestBytes) {
    error = ManifestError::kTooLarge;
    return std::nullopt;
  }

  // The non-throwing overload. An update client is the last place in this program
  // that should be able to terminate on a document somebody else wrote.
  const Json root = Json::parse(json, nullptr, false);
  if (root.is_discarded()) {
    error = ManifestError::kNotJson;
    return std::nullopt;
  }
  if (!root.is_object()) {
    error = ManifestError::kNotAnObject;
    return std::nullopt;
  }

  Manifest manifest;

  // The schema first, and before anything else is even looked at. Every field
  // below means what schema 1 says it means, and reading them out of a schema 2
  // document would be reading a different document with the same field names.
  if (!root.contains("schema") || !root["schema"].is_number_integer()) {
    error = ManifestError::kUnsupportedSchema;
    return std::nullopt;
  }
  manifest.schema = root["schema"].get<int>();
  if (manifest.schema != kManifestSchema) {
    error = ManifestError::kUnsupportedSchema;
    return std::nullopt;
  }

  if (!root.contains("channel") || !root["channel"].is_string()) {
    error = ManifestError::kBadChannel;
    return std::nullopt;
  }
  manifest.channel = root["channel"].get<std::string>();
  if (!IsAcceptablePlatform(manifest.channel)) {
    error = ManifestError::kBadChannel;
    return std::nullopt;
  }

  // Informational, and not trusted for anything: a clock on a runner is not a
  // fact. It exists so that a person looking at a stale manifest can tell.
  if (root.contains("generated_at_ms")) {
    if (!root["generated_at_ms"].is_number_integer()) {
      error = ManifestError::kNotJson;
      return std::nullopt;
    }
    manifest.generated_at_ms = root["generated_at_ms"].get<std::int64_t>();
  }

  if (!root.contains("releases") || !root["releases"].is_array()) {
    error = ManifestError::kBadReleases;
    return std::nullopt;
  }
  const Json& releases = root["releases"];
  if (releases.size() > kMaxReleases) {
    error = ManifestError::kTooMany;
    return std::nullopt;
  }

  for (const Json& node : releases) {
    if (!node.is_object()) {
      error = ManifestError::kBadReleases;
      return std::nullopt;
    }
    Release release;
    if (!node.contains("version") || !ReadVersion(node["version"], release.version)) {
      error = ManifestError::kBadVersion;
      return std::nullopt;
    }
    if (!node.contains("platform") || !node["platform"].is_string()) {
      error = ManifestError::kBadPlatform;
      return std::nullopt;
    }
    release.platform = node["platform"].get<std::string>();
    if (!IsAcceptablePlatform(release.platform)) {
      error = ManifestError::kBadPlatform;
      return std::nullopt;
    }
    if (!node.contains("archive") ||
        !ReadArtifact(node["archive"], false, release.archive, error)) {
      if (error == ManifestError::kNone) {
        error = ManifestError::kBadArtifact;
      }
      return std::nullopt;
    }
    if (!node.contains("package") ||
        !ReadArtifact(node["package"], true, release.package, error)) {
      if (error == ManifestError::kNone) {
        error = ManifestError::kBadArtifact;
      }
      return std::nullopt;
    }

    if (node.contains("deltas")) {
      if (!node["deltas"].is_array()) {
        error = ManifestError::kBadDelta;
        return std::nullopt;
      }
      if (node["deltas"].size() > kMaxDeltasPerRelease) {
        error = ManifestError::kTooMany;
        return std::nullopt;
      }
      for (const Json& delta_node : node["deltas"]) {
        if (!delta_node.is_object()) {
          error = ManifestError::kBadDelta;
          return std::nullopt;
        }
        DeltaEntry delta;
        if (!delta_node.contains("from") || !ReadVersion(delta_node["from"], delta.from)) {
          error = ManifestError::kBadVersion;
          return std::nullopt;
        }
        // A patch from a version that is not older is either a downgrade dressed
        // as an update or a generator bug, and both deserve the same answer.
        if (!(delta.from < release.version)) {
          error = ManifestError::kBadDelta;
          return std::nullopt;
        }
        const bool repeated =
            std::any_of(release.deltas.begin(), release.deltas.end(),
                        [&delta](const DeltaEntry& seen) { return seen.from == delta.from; });
        if (repeated) {
          error = ManifestError::kBadDelta;
          return std::nullopt;
        }
        if (!ReadArtifact(delta_node, true, delta.artifact, error)) {
          if (error == ManifestError::kNone) {
            error = ManifestError::kBadArtifact;
          }
          return std::nullopt;
        }
        release.deltas.push_back(std::move(delta));
      }
    }

    const bool duplicate = std::any_of(
        manifest.releases.begin(), manifest.releases.end(), [&release](const Release& seen) {
          return seen.version == release.version && seen.platform == release.platform;
        });
    if (duplicate) {
      error = ManifestError::kDuplicateRelease;
      return std::nullopt;
    }
    manifest.releases.push_back(std::move(release));
  }

  return manifest;
}

std::optional<UpdateTarget> ChooseUpdate(const Manifest& manifest,
                                         std::string_view platform,
                                         const Version& current,
                                         std::span<const Version> refused) {
  const Release* best = nullptr;
  for (const Release& release : manifest.releases) {
    if (release.platform != platform) {
      continue;
    }
    if (!(current < release.version)) {
      continue;
    }
    const bool is_refused =
        std::any_of(refused.begin(), refused.end(),
                    [&release](const Version& v) { return v == release.version; });
    if (is_refused) {
      continue;
    }
    if (best == nullptr || best->version < release.version) {
      best = &release;
    }
  }
  if (best == nullptr) {
    return std::nullopt;
  }

  UpdateTarget target;
  target.version = best->version;
  target.package = best->package;
  target.archive_size = best->archive.size;
  target.archive_hash = best->archive.hash;
  for (const DeltaEntry& delta : best->deltas) {
    if (delta.from == current) {
      target.delta = delta.artifact;
      break;
    }
  }
  return target;
}

}  // namespace sonora::update
