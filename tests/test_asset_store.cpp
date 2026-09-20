#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include <sonora/assets/asset_store.h>
#include <sonora/assets/embedded_assets.h>

using sonora::assets::MakeEmbeddedAssetStore;
using sonora::assets::MakeFilesystemAssetStore;
using sonora::assets::MimeTypeForPath;

namespace {

// A throwaway directory tree, so these tests do not depend on anyone having
// run `npm run build` first. The filesystem store is the development path and
// the one with the security-relevant logic, so it is worth testing properly.
class TempTree {
 public:
  TempTree() {
    std::random_device device;
    root_ =
        std::filesystem::temp_directory_path() / ("sonora-assets-" + std::to_string(device()));
    std::filesystem::create_directories(root_ / "assets");
    Write("index.html", "<!doctype html><title>t</title>");
    Write("assets/app.js", "export const x = 1;");
    Write("secret.txt", "not served through a parent path");
  }

  ~TempTree() {
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  TempTree(const TempTree&) = delete;
  TempTree& operator=(const TempTree&) = delete;

  [[nodiscard]] const std::filesystem::path& root() const { return root_; }

 private:
  void Write(const std::string& relative, const std::string& content) {
    std::ofstream out(root_ / relative, std::ios::binary);
    out << content;
  }

  std::filesystem::path root_;
};

}  // namespace

TEST_CASE("mime types are resolved from the extension", "[assets]") {
  REQUIRE(MimeTypeForPath("/index.html") == "text/html");
  REQUIRE(MimeTypeForPath("/assets/app.js") == "text/javascript");
  REQUIRE(MimeTypeForPath("/assets/app.css") == "text/css");
  REQUIRE(MimeTypeForPath("/fonts/inter.woff2") == "font/woff2");
}

TEST_CASE("mime type matching ignores case", "[assets]") {
  REQUIRE(MimeTypeForPath("/LOGO.PNG") == "image/png");
}

TEST_CASE("an unknown or absent extension is not guessed", "[assets]") {
  // Guessing here is how a text file ends up executed as a script. The handler
  // also sends X-Content-Type-Options: nosniff for the same reason.
  REQUIRE(MimeTypeForPath("/data.bin") == "application/octet-stream");
  REQUIRE(MimeTypeForPath("/noextension") == "application/octet-stream");
}

TEST_CASE("the filesystem store serves files under its root", "[assets]") {
  const TempTree tree;
  const auto store = MakeFilesystemAssetStore(tree.root());

  const auto index = store->Lookup("/index.html");
  REQUIRE(index.has_value());
  REQUIRE(index->mime_type == "text/html");
  REQUIRE(index->data == "<!doctype html><title>t</title>");

  // Data read from disk must be kept alive by the asset itself.
  REQUIRE(index->owner != nullptr);

  REQUIRE(store->Lookup("/assets/app.js").has_value());
}

TEST_CASE("the filesystem store refuses to escape its root", "[assets][security]") {
  // A scheme handler is a file server. A file server that follows '..' serves
  // the whole disk, and the page asking for it is web content.
  const TempTree tree;
  const auto store = MakeFilesystemAssetStore(tree.root());

  REQUIRE_FALSE(store->Lookup("/../secret.txt").has_value());
  REQUIRE_FALSE(store->Lookup("/assets/../../secret.txt").has_value());
  REQUIRE_FALSE(store->Lookup("/../../../../etc/passwd").has_value());
}

TEST_CASE("the filesystem store serves files, not directories", "[assets]") {
  const TempTree tree;
  const auto store = MakeFilesystemAssetStore(tree.root());

  REQUIRE_FALSE(store->Lookup("/assets").has_value());
  REQUIRE_FALSE(store->Lookup("/").has_value());
}

TEST_CASE("a missing file is a miss, not a crash", "[assets]") {
  const TempTree tree;
  const auto store = MakeFilesystemAssetStore(tree.root());

  REQUIRE_FALSE(store->Lookup("/nothing-here.html").has_value());
}

TEST_CASE("the embedded store is usable even with an empty table", "[assets]") {
  // A checkout where nobody has run `npm run build` yet still has to link and
  // run; the generator emits an empty table for exactly that case.
  const auto store = MakeEmbeddedAssetStore();

  REQUIRE_FALSE(store->Describe().empty());
  REQUIRE_FALSE(store->Lookup("/definitely-not-embedded.html").has_value());
}

TEST_CASE("the embedded store serves the built UI when there is one", "[assets]") {
  if (sonora::assets::embedded::kEntryCount == 0) {
    SUCCEED("no UI built into this binary; run npm run build in ui/");
    return;
  }

  const auto store = MakeEmbeddedAssetStore();
  const auto index = store->Lookup("/index.html");
  REQUIRE(index.has_value());
  REQUIRE(index->mime_type == "text/html");

  // Embedded bytes live in static storage: nothing to keep alive, nothing to
  // copy on every request.
  REQUIRE(index->owner == nullptr);
}
