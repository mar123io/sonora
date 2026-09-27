// The three tests the roadmap asked for, end to end: an update that happens, a
// corrupt one that does not, and a version that will not start and is rolled back.
//
// "End to end" here means every line of the updater except the HTTP client: a real
// directory tree on a real filesystem, a real archive, a real Ed25519 signature over
// a real manifest, a real zstd patch, real renames, and the real journal driving all
// of it. The Fetcher reads from a directory instead of a socket, which is the same
// substitution the unit tests make for the filesystem, and for the same reason --
// what is left untested is the twenty lines that call WinHTTP, and what is tested is
// everything that could destroy somebody's installation.
//
// It runs on Windows, macOS and Linux, which the roadmap's version of this test would
// not have.

#include <sodium.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "sonora/update/archive.h"
#include "sonora/update/filestore.h"
#include "sonora/update/hash.h"
#include "sonora/update/journal.h"
#include "sonora/update/patch.h"
#include "sonora/update/signature.h"
#include "sonora/update/updater.h"

namespace {

namespace fs = std::filesystem;
using namespace sonora::update;

constexpr Version kV1{1, 0, 0};
constexpr Version kV2{1, 0, 1};
constexpr std::string_view kPlatform = "win-x64";
constexpr std::string_view kManifestUrl = "https://updates.test/stable/manifest.json";
constexpr std::string_view kSignatureUrl = "https://updates.test/stable/manifest.json.sig";
constexpr std::string_view kPackageUrl = "https://updates.test/stable/Sonora-1.0.1.spk.zst";
constexpr std::string_view kDeltaUrl = "https://updates.test/stable/1.0.0-1.0.1.patch";

std::vector<std::uint8_t> Noise(std::size_t size, std::uint32_t seed) {
  std::mt19937 engine(seed);
  std::vector<std::uint8_t> bytes(size);
  for (std::uint8_t& byte : bytes) {
    byte = static_cast<std::uint8_t>(engine() & 0xFF);
  }
  return bytes;
}

void Write(const fs::path& path, std::span<const std::uint8_t> bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  out.close();
  REQUIRE(out);
}

void WriteText(const fs::path& path, std::string_view text) {
  Write(path, {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

std::vector<std::uint8_t> Read(const fs::path& path) {
  FileStoreError error = FileStoreError::kNone;
  auto bytes = ReadFile(path, 1ull << 30, error);
  REQUIRE(bytes.has_value());
  return *bytes;
}

// A payload that looks like Sonora's in the ways that matter: a large file that does
// not change between versions, a smaller one that does, and some files in
// subdirectories.
void BuildPayload(const fs::path& dir, bool second_version) {
  Write(dir / "libcef.dll", Noise(400 * 1024, 100));  // unchanged across versions
  Write(dir / "Sonora.exe", Noise(64 * 1024, second_version ? 2 : 1));
  Write(dir / "locales" / "it.pak", Noise(4096, 7));
  Write(dir / "swiftshader" / "libvk_swiftshader.dll", Noise(32 * 1024, 8));
  WriteText(dir / "version.txt", second_version ? "1.0.1" : "1.0.0");
}

// Every file under `dir`, by relative path, so that two trees can be compared as
// values rather than as directories.
std::map<std::string, std::vector<std::uint8_t>> Snapshot(const fs::path& dir) {
  std::map<std::string, std::vector<std::uint8_t>> files;
  if (!fs::is_directory(dir)) {
    return files;
  }
  for (const fs::directory_entry& entry : fs::recursive_directory_iterator(dir)) {
    if (entry.is_regular_file()) {
      files[fs::relative(entry.path(), dir).generic_string()] = Read(entry.path());
    }
  }
  return files;
}

// A Fetcher over a std::map, which is what the release directory is once the urls in
// the manifest are the keys.
class MapFetcher final : public Fetcher {
 public:
  std::map<std::string, std::vector<std::uint8_t>> responses;
  int calls = 0;

  std::optional<std::vector<std::uint8_t>> Get(std::string_view url,
                                               std::uint64_t max_bytes) override {
    ++calls;
    const auto found = responses.find(std::string(url));
    if (found == responses.end()) {
      return std::nullopt;
    }
    if (found->second.size() > max_bytes) {
      return std::nullopt;  // the cap in the signed manifest, enforced here
    }
    return found->second;
  }
};

std::string Hex(std::span<const std::uint8_t> bytes) {
  return ToHex(bytes);
}

// Everything a release produces, plus the key that signed it.
struct World {
  fs::path base;
  Layout layout;
  MapFetcher fetcher;
  UpdateConfig config;
  std::vector<PublicKey> keys;
  std::map<std::string, std::vector<std::uint8_t>> payload_v1;
  std::map<std::string, std::vector<std::uint8_t>> payload_v2;
  std::uint64_t package_size = 0;
  std::uint64_t delta_size = 0;
};

// Builds two releases, signs a manifest naming both, and installs the first.
//
// `lie_about_the_archive` makes the manifest name the right package and the wrong
// uncompressed archive -- a release whose publisher's own bookkeeping is inconsistent. It
// exists because the mutation run found that nothing else reaches the last of the three
// hash checks: the package's hash catches a corrupt download, the reconstruction's hash
// catches a modified installation, and only this reaches the one on the archive itself.
World MakeWorld(const fs::path& base,
                bool lie_about_the_archive = false,
                bool lie_about_the_package = false) {
  REQUIRE(sodium_init() >= 0);

  World world;
  world.base = base;
  world.layout.root = base / "install";
  fs::create_directories(world.layout.root);
  const fs::path build = base / "build";

  // The two payload trees, as a release job would have them in its staging directory.
  BuildPayload(build / "1.0.0", false);
  BuildPayload(build / "1.0.1", true);
  world.payload_v1 = Snapshot(build / "1.0.0");
  world.payload_v2 = Snapshot(build / "1.0.1");

  FileStoreError store_error = FileStoreError::kNone;
  const auto packed_v1 = PackDirectory(build / "1.0.0", base / "1.0.0.spk", store_error);
  const auto packed_v2 = PackDirectory(build / "1.0.1", base / "1.0.1.spk", store_error);
  REQUIRE(packed_v1.has_value());
  REQUIRE(packed_v2.has_value());

  const std::vector<std::uint8_t> spk_v1 = Read(base / "1.0.0.spk");
  const std::vector<std::uint8_t> spk_v2 = Read(base / "1.0.1.spk");

  PatchError patch_error = PatchError::kNone;
  const auto package = CompressFrame(spk_v2, patch_error, 9);
  REQUIRE(package.has_value());
  const auto delta = CreatePatch(spk_v1, spk_v2, patch_error, 9);
  REQUIRE(delta.has_value());
  world.package_size = package->size();
  world.delta_size = delta->size();

  const auto hash_v1 = HashBytes(spk_v1);
  auto hash_v2 = HashBytes(spk_v2);
  if (lie_about_the_archive) {
    REQUIRE(hash_v2.has_value());
    (*hash_v2)[0] ^= 0xFF;
  }
  auto hash_package = HashBytes(*package);
  if (lie_about_the_package) {
    REQUIRE(hash_package.has_value());
    (*hash_package)[0] ^= 0xFF;
  }
  const auto hash_delta = HashBytes(*delta);
  REQUIRE(hash_v2.has_value());

  // The manifest, written the way the release workflow writes it: both versions
  // listed, so that the client can check its own installation against the release it
  // came from before patching.
  const std::string manifest =
      std::string("{\n  \"schema\": 1,\n  \"channel\": \"stable\",\n  \"releases\": [\n") +
      "    {\"version\": \"1.0.0\", \"platform\": \"win-x64\",\n" +
      "     \"archive\": {\"size\": " + std::to_string(spk_v1.size()) + ", \"hash\": \"" +
      Hex(*hash_v1) + "\"},\n" +
      "     \"package\": {\"url\": \"https://updates.test/stable/Sonora-1.0.0.spk.zst\", " +
      "\"size\": 1, \"hash\": \"" + Hex(*hash_v1) + "\"}},\n" +
      "    {\"version\": \"1.0.1\", \"platform\": \"win-x64\",\n" +
      "     \"archive\": {\"size\": " + std::to_string(spk_v2.size()) + ", \"hash\": \"" +
      Hex(*hash_v2) + "\"},\n" + "     \"package\": {\"url\": \"" + std::string(kPackageUrl) +
      "\", \"size\": " + std::to_string(package->size()) + ", \"hash\": \"" +
      Hex(*hash_package) + "\"},\n" + "     \"deltas\": [{\"from\": \"1.0.0\", \"url\": \"" +
      std::string(kDeltaUrl) + "\", \"size\": " + std::to_string(delta->size()) +
      ", \"hash\": \"" + Hex(*hash_delta) + "\"}]}\n  ]\n}";

  // A signing key made here and now. The release key's private half lives in a
  // repository secret and is not in this tree (ADR 0009), and a test that needed it
  // would be a test that could not run.
  std::vector<std::uint8_t> public_key(crypto_sign_PUBLICKEYBYTES);
  std::vector<std::uint8_t> secret_key(crypto_sign_SECRETKEYBYTES);
  REQUIRE(crypto_sign_keypair(public_key.data(), secret_key.data()) == 0);
  std::vector<std::uint8_t> signature(crypto_sign_BYTES);
  REQUIRE(crypto_sign_detached(signature.data(), nullptr,
                               reinterpret_cast<const std::uint8_t*>(manifest.data()),
                               manifest.size(), secret_key.data()) == 0);

  PublicKey key{};
  std::copy(public_key.begin(), public_key.end(), key.begin());
  world.keys.push_back(key);

  world.config.manifest_url = std::string(kManifestUrl);
  world.config.signature_url = std::string(kSignatureUrl);
  world.config.platform = std::string(kPlatform);
  world.config.keys = world.keys;

  world.fetcher.responses[std::string(kManifestUrl)] = {manifest.begin(), manifest.end()};
  const std::string signature_hex = ToHex(signature) + "\n";
  world.fetcher.responses[std::string(kSignatureUrl)] = {signature_hex.begin(),
                                                         signature_hex.end()};
  world.fetcher.responses[std::string(kPackageUrl)] = *package;
  world.fetcher.responses[std::string(kDeltaUrl)] = *delta;

  // And 1.0.0 is what is installed, exactly as its package describes it.
  fs::copy(build / "1.0.0", world.layout.current(), fs::copy_options::recursive);
  REQUIRE(WriteLaunchFlag(world.layout, kV1, NoFlush()));
  return world;
}

// A temporary directory that cleans up after itself, wherever the machine keeps them.
class TempDir {
 public:
  TempDir() {
    std::mt19937 engine(std::random_device{}());
    path_ = fs::temp_directory_path() / ("sonora-e2e-" + std::to_string(engine()));
    fs::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  [[nodiscard]] const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

// One start of the application, as ADR 0011 describes it: recover, and if the
// recovery needs to move the installation, hand over to the out-of-tree copy -- which
// here is the same call with may_touch_installation set, because a test does not need
// a second process to prove that the second process would work.
StartupOutcome Start(const Layout& layout) {
  const StartupOutcome in_process = RecoverAtStartup(layout, NoFlush(), false);
  if (in_process.result != DriveResult::kNeedsOutOfProcess) {
    return in_process;
  }
  return RecoverAtStartup(layout, NoFlush(), true);
}

}  // namespace

TEST_CASE("end to end: an update is offered, taken as a delta, and installed") {
  TempDir temp;
  World world = MakeWorld(temp.path());

  const StageOutcome staged =
      CheckAndStage(world.layout, world.fetcher, world.config, kV1, NoFlush());
  REQUIRE(staged.result == StageResult::kStaged);
  CHECK(staged.version == kV2);
  CHECK(staged.used_delta);

  // The number this whole week exists for. The delta is a fraction of the package,
  // and the client downloaded the fraction.
  CHECK(staged.downloaded < world.package_size / 4);
  CHECK(world.delta_size < world.package_size / 4);

  // Nothing has moved yet: the staged tree is beside the installation and the
  // application is still running the old version.
  CHECK(Snapshot(world.layout.current()) == world.payload_v1);
  CHECK(Snapshot(world.layout.staged()) == world.payload_v2);

  // The swap, at the exit of that run and the start of the next.
  const StartupOutcome start = Start(world.layout);
  CHECK(start.result == DriveResult::kDone);
  CHECK(start.journal.stage == Stage::kUnconfirmed);
  CHECK(Snapshot(world.layout.current()) == world.payload_v2);
  CHECK(fs::is_directory(world.layout.previous()));

  // The new version starts and says so, and the start after that cleans up.
  REQUIRE(ConfirmLaunch(world.layout, kV2, NoFlush()));
  const StartupOutcome settled = Start(world.layout);
  CHECK(settled.result == DriveResult::kDone);
  CHECK(settled.journal.stage == Stage::kIdle);
  CHECK(settled.journal.refused.empty());
  CHECK_FALSE(fs::exists(world.layout.previous()));
  CHECK_FALSE(fs::exists(world.layout.staged()));
  CHECK(Snapshot(world.layout.current()) == world.payload_v2);

  // ...and there is nothing left to do.
  const StageOutcome again =
      CheckAndStage(world.layout, world.fetcher, world.config, kV2, NoFlush());
  CHECK(again.result == StageResult::kUpToDate);
}

TEST_CASE("end to end: a corrupt delta falls back to the package rather than failing") {
  TempDir temp;
  World world = MakeWorld(temp.path());
  world.fetcher.responses[std::string(kDeltaUrl)][40] ^= 0xFF;

  const StageOutcome staged =
      CheckAndStage(world.layout, world.fetcher, world.config, kV1, NoFlush());
  // The roadmap expected "no installation"; this is better and it is a decision, not
  // an accident. A corrupt delta invalidates the delta, not the release: the package
  // is still there, its hash is still in the signed manifest, and an update that
  // arrives as 47 MiB instead of 84 KiB is not a failure.
  REQUIRE(staged.result == StageResult::kStaged);
  CHECK_FALSE(staged.used_delta);
  CHECK(Snapshot(world.layout.staged()) == world.payload_v2);
}

TEST_CASE("end to end: with both the delta and the package corrupt, nothing is installed") {
  TempDir temp;
  World world = MakeWorld(temp.path());
  world.fetcher.responses[std::string(kDeltaUrl)][40] ^= 0xFF;
  world.fetcher.responses[std::string(kPackageUrl)][80] ^= 0xFF;

  const StageOutcome staged =
      CheckAndStage(world.layout, world.fetcher, world.config, kV1, NoFlush());
  CHECK(staged.result == StageResult::kArtifactInvalid);

  // The installation is untouched, the journal is idle, and there is no half-written
  // tree left behind for a later start to find.
  CHECK(Snapshot(world.layout.current()) == world.payload_v1);
  CHECK_FALSE(fs::exists(world.layout.staged()));
  const LoadedJournal journal = LoadJournal(world.layout);
  CHECK(journal.readable);
  CHECK(journal.journal.stage == Stage::kIdle);

  // And the application still starts, which is the part that matters to the person
  // using it.
  const StartupOutcome start = Start(world.layout);
  CHECK(start.result == DriveResult::kDone);
  CHECK(Snapshot(world.layout.current()) == world.payload_v1);
}

TEST_CASE("end to end: a version that will not start is rolled back automatically") {
  TempDir temp;
  World world = MakeWorld(temp.path());

  REQUIRE(CheckAndStage(world.layout, world.fetcher, world.config, kV1, NoFlush()).result ==
          StageResult::kStaged);

  // The swap happens, and then 1.0.1 runs and never reaches the point where it would
  // write the flag -- it crashes on startup, or hangs, or the machine is turned off
  // while the splash is up. Nothing here distinguishes those, and nothing needs to.
  CHECK(Start(world.layout).journal.stage == Stage::kUnconfirmed);
  CHECK(Snapshot(world.layout.current()) == world.payload_v2);

  const StartupOutcome attempt = Start(world.layout);
  CHECK(attempt.journal.attempts == 1);
  CHECK(Snapshot(world.layout.current()) == world.payload_v2);  // one chance, not none

  const StartupOutcome rolled_back = Start(world.layout);
  CHECK(rolled_back.journal.stage == Stage::kIdle);
  CHECK(rolled_back.journal.refused == std::vector<Version>{kV2});
  // Byte for byte the tree that was there before the update.
  CHECK(Snapshot(world.layout.current()) == world.payload_v1);
  CHECK_FALSE(fs::exists(world.layout.previous()));

  // And it is not offered again, so the machine does not spend the rest of its life
  // downloading, installing and rolling back the same version every six hours.
  const StageOutcome after =
      CheckAndStage(world.layout, world.fetcher, world.config, kV1, NoFlush());
  CHECK(after.result == StageResult::kUpToDate);
}

TEST_CASE("end to end: a manifest with one byte changed is not read at all") {
  TempDir temp;
  World world = MakeWorld(temp.path());
  auto& manifest = world.fetcher.responses[std::string(kManifestUrl)];
  // The size of the package: what somebody who could rewrite the file but not sign it
  // would want to change.
  const auto at = std::find(manifest.begin(), manifest.end(), static_cast<std::uint8_t>('7'));
  REQUIRE(at != manifest.end());
  *at = '8';

  const StageOutcome staged =
      CheckAndStage(world.layout, world.fetcher, world.config, kV1, NoFlush());
  CHECK(staged.result == StageResult::kSignatureInvalid);
  CHECK_FALSE(fs::exists(world.layout.staged()));
  CHECK(Snapshot(world.layout.current()) == world.payload_v1);
}

TEST_CASE("end to end: a signature by a key this build does not trust is not a signature") {
  TempDir temp;
  World world = MakeWorld(temp.path());
  std::vector<PublicKey> stranger(1);
  stranger[0].fill(0x42);
  world.config.keys = stranger;

  CHECK(CheckAndStage(world.layout, world.fetcher, world.config, kV1, NoFlush()).result ==
        StageResult::kSignatureInvalid);
}

TEST_CASE(
    "end to end: an installation that is not what it was published as takes the package") {
  TempDir temp;
  World world = MakeWorld(temp.path());

  // One byte of one installed file, changed by something that is not this updater: a
  // scanner's quarantine-and-restore, a half-finished copy, a file somebody edited.
  // The reconstruction of the archive then does not hash to what the manifest says,
  // and patching from it would produce garbage of exactly the right length.
  const fs::path victim = world.layout.current() / "locales" / "it.pak";
  std::vector<std::uint8_t> bytes = Read(victim);
  bytes[10] ^= 0xFF;
  Write(victim, bytes);

  const StageOutcome staged =
      CheckAndStage(world.layout, world.fetcher, world.config, kV1, NoFlush());
  REQUIRE(staged.result == StageResult::kStaged);
  CHECK_FALSE(staged.used_delta);
  CHECK(staged.detail == "the installed version does not match its published package");
  CHECK(Snapshot(world.layout.staged()) == world.payload_v2);
}

TEST_CASE("end to end: an update already in flight is not started twice") {
  TempDir temp;
  World world = MakeWorld(temp.path());
  REQUIRE(CheckAndStage(world.layout, world.fetcher, world.config, kV1, NoFlush()).result ==
          StageResult::kStaged);

  const int calls_before = world.fetcher.calls;
  const StageOutcome second =
      CheckAndStage(world.layout, world.fetcher, world.config, kV1, NoFlush());
  CHECK(second.result == StageResult::kBusy);
  // Not even the manifest: a check that cannot act on its answer should not ask the
  // question, and every installation asking every six hours adds up.
  CHECK(world.fetcher.calls == calls_before);
}

TEST_CASE("end to end: an unreadable journal stops the updater instead of guessing") {
  TempDir temp;
  World world = MakeWorld(temp.path());
  WriteText(world.layout.journal_file(), "{ this is not a journal");

  CHECK(CheckAndStage(world.layout, world.fetcher, world.config, kV1, NoFlush()).result ==
        StageResult::kJournalUnreadable);
  const StartupOutcome start = Start(world.layout);
  CHECK(start.journal_unreadable);
  CHECK(Snapshot(world.layout.current()) == world.payload_v1);
}

TEST_CASE("an archive round-trips a real directory tree byte for byte") {
  TempDir temp;
  const fs::path source = temp.path() / "tree";
  BuildPayload(source, false);

  FileStoreError error = FileStoreError::kNone;
  const auto packed = PackDirectory(source, temp.path() / "a.spk", error);
  REQUIRE(packed.has_value());
  REQUIRE(UnpackArchive(temp.path() / "a.spk", temp.path() / "out", NoFlush(), error));
  CHECK(Snapshot(source) == Snapshot(temp.path() / "out"));

  // ...and packing the unpacked copy gives the same bytes, which is the property the
  // delta path depends on: a client can rebuild the archive it was installed from.
  const auto again = PackDirectory(temp.path() / "out", temp.path() / "b.spk", error);
  REQUIRE(again.has_value());
  CHECK(Read(temp.path() / "a.spk") == Read(temp.path() / "b.spk"));
}

TEST_CASE("unpacking refuses to write where something already is") {
  TempDir temp;
  const fs::path source = temp.path() / "tree";
  BuildPayload(source, false);
  FileStoreError error = FileStoreError::kNone;
  REQUIRE(PackDirectory(source, temp.path() / "a.spk", error).has_value());

  fs::create_directories(temp.path() / "out");
  CHECK_FALSE(UnpackArchive(temp.path() / "a.spk", temp.path() / "out", NoFlush(), error));
  CHECK(error == FileStoreError::kAlreadyExists);
}

TEST_CASE("a truncated package is not unpacked") {
  TempDir temp;
  const fs::path source = temp.path() / "tree";
  BuildPayload(source, false);
  FileStoreError error = FileStoreError::kNone;
  REQUIRE(PackDirectory(source, temp.path() / "a.spk", error).has_value());

  std::vector<std::uint8_t> bytes = Read(temp.path() / "a.spk");
  bytes.resize(bytes.size() - 1);
  Write(temp.path() / "short.spk", bytes);
  CHECK_FALSE(UnpackArchive(temp.path() / "short.spk", temp.path() / "out", NoFlush(), error));
  CHECK(error == FileStoreError::kArchiveInvalid);
}

TEST_CASE("a package whose contents do not match its own hashes is not unpacked") {
  TempDir temp;
  const fs::path source = temp.path() / "tree";
  BuildPayload(source, false);
  FileStoreError error = FileStoreError::kNone;
  const auto packed = PackDirectory(source, temp.path() / "a.spk", error);
  REQUIRE(packed.has_value());

  // A byte in the contents, not in the header: the length still adds up, so only the
  // per-member hash can tell.
  std::vector<std::uint8_t> bytes = Read(temp.path() / "a.spk");
  bytes[static_cast<std::size_t>(packed->content_offset) + 5] ^= 0xFF;
  Write(temp.path() / "bad.spk", bytes);
  CHECK_FALSE(UnpackArchive(temp.path() / "bad.spk", temp.path() / "out", NoFlush(), error));
  CHECK(error == FileStoreError::kHashMismatch);
}

TEST_CASE("the journal is either the old one or the new one, never half of one") {
  TempDir temp;
  Layout layout;
  layout.root = temp.path();

  Journal first;
  first.stage = Stage::kStaged;
  first.from = kV1;
  first.to = kV2;
  FileSystemRunner runner(layout, NoFlush());
  REQUIRE(runner.WriteJournal(first));
  CHECK(LoadJournal(layout).journal == first);

  // The temporary file is gone: a leftover would be read by nothing but would sit in
  // the user's profile forever.
  CHECK_FALSE(fs::exists(fs::path(layout.journal_file()) += ".tmp"));

  Journal second = first;
  second.stage = Stage::kUnconfirmed;
  second.attempts = 1;
  REQUIRE(runner.WriteJournal(second));
  CHECK(LoadJournal(layout).journal == second);
}

TEST_CASE("end to end: a package that is not the archive the manifest names is refused") {
  TempDir temp;
  World world = MakeWorld(temp.path(), /*lie_about_the_archive=*/true);

  // The package downloads, its own hash matches, it decompresses -- and what comes out is
  // not what the signed manifest said the archive would be. The delta path is skipped for
  // the same reason, so both roads lead here.
  const StageOutcome staged =
      CheckAndStage(world.layout, world.fetcher, world.config, kV1, NoFlush());
  CHECK(staged.result == StageResult::kArtifactInvalid);
  CHECK_FALSE(fs::exists(world.layout.staged()));
  CHECK(Snapshot(world.layout.current()) == world.payload_v1);
}

TEST_CASE("a write whose flush fails leaves the previous contents alone") {
  TempDir temp;
  Layout layout;
  layout.root = temp.path();

  Journal good;
  good.stage = Stage::kStaged;
  good.from = kV1;
  good.to = kV2;
  FileSystemRunner writer(layout, NoFlush());
  REQUIRE(writer.WriteJournal(good));

  // A flush that says no stands in for the machine stopping between the write and the
  // rename. What must not happen is the old journal being gone: it is the only record of
  // what is in flight, and losing it mid-swap is the one failure this design cannot
  // survive. This is also the only test that can tell the temporary file is there -- see
  // the note in WriteFileDurably.
  FileSystemRunner failing(layout, [](const fs::path&) { return false; });
  Journal other = good;
  other.stage = Stage::kRollingBack;
  CHECK_FALSE(failing.WriteJournal(other));

  const LoadedJournal loaded = LoadJournal(layout);
  CHECK(loaded.readable);
  CHECK(loaded.journal == good);
  CHECK_FALSE(fs::exists(fs::path(layout.journal_file()) += ".tmp"));
}

TEST_CASE("a symbolic link in a payload is refused rather than followed") {
  TempDir temp;
  const fs::path source = temp.path() / "tree";
  BuildPayload(source, false);

  std::error_code ec;
  fs::create_symlink(source / "libcef.dll", source / "link.dll", ec);
  if (ec) {
    // Windows needs a privilege for this and a test is not the place to ask for one. The
    // rule still holds there; what cannot be demonstrated there is demonstrated here, and
    // CI runs this job on Linux and macOS too.
    SUCCEED("this machine will not create a symbolic link; nothing to check");
    return;
  }

  // Following it would copy the 400 KiB target under a second name and produce a package
  // that is not the tree; refusing to follow it but recording the link would produce a
  // member that is not a file. Neither is a payload, so this is not a packaging choice --
  // it is a release that must not be built. See filestore.h.
  FileStoreError error = FileStoreError::kNone;
  CHECK_FALSE(PackDirectory(source, temp.path() / "a.spk", error).has_value());
  CHECK(error == FileStoreError::kSymlink);
}

TEST_CASE("the executable flag means something, or is not there at all") {
  TempDir temp;
  const fs::path source = temp.path() / "tree";
  BuildPayload(source, false);

  std::error_code ec;
  fs::permissions(source / "Sonora.exe", fs::perms::owner_exec, fs::perm_options::add, ec);

  FileStoreError error = FileStoreError::kNone;
  const auto packed = PackDirectory(source, temp.path() / "a.spk", error);
  REQUIRE(packed.has_value());

  // Two filesystems, two assertions, and neither of them is a skip.
  //
  // The first version of this case checked whether a file it had just chmod'd came back
  // executable and gave up if it did not. On Windows it did -- MSVC reports perms::all for
  // every writable file -- so the case ran, and then failed on the file that was *not*
  // supposed to be executable. That failure was right: without the probe, every member of
  // a payload packed on Windows carried the flag, including the .pak files.
  if (!ExecuteBitIsMeaningful(temp.path())) {
    for (const Member& member : packed->members) {
      CHECK((member.flags & kMemberExecutable) == 0);
    }
    return;
  }

  const auto executable =
      std::find_if(packed->members.begin(), packed->members.end(),
                   [](const Member& candidate) { return candidate.path == "Sonora.exe"; });
  REQUIRE(executable != packed->members.end());
  CHECK((executable->flags & kMemberExecutable) != 0);

  REQUIRE(UnpackArchive(temp.path() / "a.spk", temp.path() / "out", NoFlush(), error));
  CHECK((fs::status(temp.path() / "out" / "Sonora.exe", ec).permissions() &
         fs::perms::owner_exec) != fs::perms::none);
  // ...and a file that was not executable does not become one, which is the half that an
  // implementation marking everything would also pass.
  CHECK((fs::status(temp.path() / "out" / "locales" / "it.pak", ec).permissions() &
         fs::perms::owner_exec) == fs::perms::none);
}

TEST_CASE("end to end: a package whose hash is not the one the manifest names is refused") {
  TempDir temp;
  World world = MakeWorld(temp.path(), /*lie_about_the_archive=*/false,
                          /*lie_about_the_package=*/true);
  // The delta has to be out of the way for the package path to be the one taken, so it is
  // corrupted -- which its own hash catches, exactly as the fallback test above shows.
  world.fetcher.responses[std::string(kDeltaUrl)][40] ^= 0xFF;

  // What is left is a package that downloads, decompresses cleanly and produces the right
  // archive, named by a manifest that says its hash is something else. Nothing but the
  // hash check on the package can tell: zstd's own frame checksum is happy, and the
  // archive that comes out of it is the right one.
  //
  // This case exists because the mutation run found that removing that check changed
  // nothing -- every other test reached it through a corruption that zstd caught first.
  const StageOutcome staged =
      CheckAndStage(world.layout, world.fetcher, world.config, kV1, NoFlush());
  CHECK(staged.result == StageResult::kArtifactInvalid);
  CHECK(staged.detail == "the package is not the package the manifest names");
  CHECK_FALSE(fs::exists(world.layout.staged()));
  CHECK(Snapshot(world.layout.current()) == world.payload_v1);
}
