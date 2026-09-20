#include <sonora/assets/asset_store.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <utility>

#include <sonora/assets/embedded_assets.h>

namespace sonora::assets {
namespace {

std::string ToLowerExtension(std::string_view path) {
  const auto dot = path.rfind('.');
  if (dot == std::string_view::npos) {
    return {};
  }
  std::string ext(path.substr(dot));
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return ext;
}

class EmbeddedAssetStore final : public AssetStore {
 public:
  EmbeddedAssetStore() {
    index_.reserve(embedded::kEntryCount);
    for (std::size_t i = 0; i < embedded::kEntryCount; ++i) {
      index_.emplace(embedded::kEntries[i].path, &embedded::kEntries[i]);
    }
  }

  std::optional<Asset> Lookup(std::string_view path) const override {
    const auto it = index_.find(std::string(path));
    if (it == index_.end()) {
      return std::nullopt;
    }
    const embedded::Entry* entry = it->second;
    return Asset{
        std::string_view(reinterpret_cast<const char*>(entry->data), entry->size),
        entry->mime_type,
        nullptr,  // static storage, nothing to keep alive
    };
  }

  std::string Describe() const override {
    return "embedded (" + std::to_string(embedded::kEntryCount) + " files)";
  }

 private:
  std::unordered_map<std::string, const embedded::Entry*> index_;
};

class FilesystemAssetStore final : public AssetStore {
 public:
  explicit FilesystemAssetStore(std::filesystem::path root) : root_(std::move(root)) {}

  std::optional<Asset> Lookup(std::string_view path) const override {
    const auto resolved = Resolve(path);
    if (!resolved) {
      return std::nullopt;
    }
    std::ifstream file(*resolved, std::ios::binary);
    if (!file) {
      return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    auto owner = std::make_shared<const std::string>(buffer.str());
    return Asset{std::string_view(*owner), MimeTypeForPath(path), owner};
  }

  std::string Describe() const override { return "filesystem (" + root_.string() + ")"; }

 private:
  // Refuses anything that escapes the root. A scheme handler is a file server,
  // and a file server that follows '..' serves the whole disk.
  std::optional<std::filesystem::path> Resolve(std::string_view path) const {
    std::error_code ec;
    const auto canonical_root = std::filesystem::weakly_canonical(root_, ec);
    if (ec) {
      return std::nullopt;
    }
    std::filesystem::path candidate = canonical_root;
    candidate += std::filesystem::path(std::string(path)).make_preferred();
    const auto canonical = std::filesystem::weakly_canonical(candidate, ec);
    if (ec) {
      return std::nullopt;
    }
    const auto root_str = canonical_root.native();
    const auto candidate_str = canonical.native();
    if (candidate_str.size() < root_str.size() ||
        candidate_str.compare(0, root_str.size(), root_str) != 0) {
      return std::nullopt;
    }
    if (!std::filesystem::is_regular_file(canonical, ec)) {
      return std::nullopt;
    }
    return canonical;
  }

  std::filesystem::path root_;
};

}  // namespace

std::string MimeTypeForPath(std::string_view path) {
  static const std::unordered_map<std::string, std::string> kByExtension = {
      {".html", "text/html"},      {".htm", "text/html"},    {".js", "text/javascript"},
      {".mjs", "text/javascript"}, {".css", "text/css"},     {".json", "application/json"},
      {".svg", "image/svg+xml"},   {".png", "image/png"},    {".jpg", "image/jpeg"},
      {".jpeg", "image/jpeg"},     {".webp", "image/webp"},  {".ico", "image/x-icon"},
      {".woff", "font/woff"},      {".woff2", "font/woff2"}, {".map", "application/json"},
      {".txt", "text/plain"},
  };
  const auto it = kByExtension.find(ToLowerExtension(path));
  return it != kByExtension.end() ? it->second : "application/octet-stream";
}

std::unique_ptr<AssetStore> MakeEmbeddedAssetStore() {
  return std::make_unique<EmbeddedAssetStore>();
}

std::unique_ptr<AssetStore> MakeFilesystemAssetStore(std::filesystem::path root) {
  return std::make_unique<FilesystemAssetStore>(std::move(root));
}

std::unique_ptr<AssetStore> MakeDefaultAssetStore() {
#if defined(SONORA_UI_DIST_DIR)
  std::error_code ec;
  const std::filesystem::path dist(SONORA_UI_DIST_DIR);
  if (std::filesystem::is_directory(dist, ec)) {
    return MakeFilesystemAssetStore(dist);
  }
#endif
  return MakeEmbeddedAssetStore();
}

}  // namespace sonora::assets
