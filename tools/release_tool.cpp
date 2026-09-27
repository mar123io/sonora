// sonora_release: what a release job runs to turn a payload directory into the things
// an installation downloads.
//
// It is a C++ program that links sonora::update, and that is the point rather than a
// preference. The archive format, the hash and the patch parameters exist in exactly
// one place, so the bytes CI publishes and the bytes a user's updater expects cannot
// drift -- and `nbWorkers = 0`, which is worth a factor of two on the size of every
// patch (ADR 0010), is a constant in a header instead of a flag somebody has to
// remember to pass.
//
//   sonora_release pack     <payload-dir> <out.spk>
//   sonora_release compress <in.spk>      <out.spk.zst>
//   sonora_release expand   <in.spk.zst>  <out.spk>
//   sonora_release delta    <old.spk>     <new.spk>   <out.patch>
//   sonora_release describe <file>
//   sonora_release inspect  <in.spk>
//
// Every subcommand prints "size=<bytes>" and "hash=<64 hex>" for what it produced, in
// a shape a workflow can read with one grep, and nothing else on standard output.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "sonora/update/archive.h"
#include "sonora/update/filestore.h"
#include "sonora/update/hash.h"
#include "sonora/update/patch.h"

namespace {

namespace fs = std::filesystem;
using namespace sonora::update;

constexpr std::uint64_t kMaxInput = 8ull * 1024 * 1024 * 1024;

int Fail(std::string_view message) {
  std::fprintf(stderr, "sonora_release: %.*s\n", static_cast<int>(message.size()),
               message.data());
  return 1;
}

int Report(const fs::path& path) {
  FileStoreError error = FileStoreError::kNone;
  std::error_code ec;
  const std::uintmax_t size = fs::file_size(path, ec);
  if (ec) {
    return Fail("cannot measure " + path.string());
  }
  const auto hash = HashFile(path, error);
  if (!hash.has_value()) {
    return Fail("cannot hash " + path.string() + ": " + std::string(Describe(error)));
  }
  std::printf("size=%llu\n", static_cast<unsigned long long>(size));
  std::printf("hash=%s\n", ToHex(*hash).c_str());
  return 0;
}

std::optional<std::vector<std::uint8_t>> Slurp(const fs::path& path) {
  FileStoreError error = FileStoreError::kNone;
  auto bytes = ReadFile(path, kMaxInput, error);
  if (!bytes.has_value()) {
    std::fprintf(stderr, "sonora_release: cannot read %s: %.*s\n", path.string().c_str(),
                 static_cast<int>(Describe(error).size()), Describe(error).data());
  }
  return bytes;
}

int Pack(const fs::path& dir, const fs::path& out) {
  FileStoreError error = FileStoreError::kNone;
  const auto header = PackDirectory(dir, out, error);
  if (!header.has_value()) {
    return Fail("cannot pack " + dir.string() + ": " + std::string(Describe(error)));
  }
  std::fprintf(stderr, "packed %zu members from %s\n", header->members.size(),
               dir.string().c_str());
  return Report(out);
}

int Compress(const fs::path& in, const fs::path& out) {
  const auto bytes = Slurp(in);
  if (!bytes.has_value()) {
    return 1;
  }
  PatchError error = PatchError::kNone;
  const auto frame = CompressFrame(*bytes, error);
  if (!frame.has_value()) {
    return Fail(Describe(error));
  }
  FileStoreError store_error = FileStoreError::kNone;
  if (!WriteFileDurably(out, *frame, NoFlush(), store_error)) {
    return Fail(Describe(store_error));
  }
  return Report(out);
}

int Delta(const fs::path& old_path, const fs::path& new_path, const fs::path& out) {
  const auto old_bytes = Slurp(old_path);
  const auto new_bytes = Slurp(new_path);
  if (!old_bytes.has_value() || !new_bytes.has_value()) {
    return 1;
  }
  PatchError error = PatchError::kNone;
  const auto patch = CreatePatch(*old_bytes, *new_bytes, error);
  if (!patch.has_value()) {
    return Fail(Describe(error));
  }

  // The generator checks its own work, because a patch that does not apply is a release
  // that silently falls back to a 47 MiB download for everybody on the old version --
  // and nothing about the release would look wrong.
  const auto rebuilt = ApplyPatch(*old_bytes, *patch, new_bytes->size(), error);
  if (!rebuilt.has_value() || *rebuilt != *new_bytes) {
    return Fail("the patch this just generated does not rebuild the new package");
  }

  FileStoreError store_error = FileStoreError::kNone;
  if (!WriteFileDurably(out, *patch, NoFlush(), store_error)) {
    return Fail(Describe(store_error));
  }
  std::fprintf(stderr, "delta is %.4f%% of the package it rebuilds\n",
               100.0 * static_cast<double>(patch->size()) / static_cast<double>(new_bytes->size()));
  return Report(out);
}

// Expanding a package back into the archive it was made from, which the delta generator
// needs because a release publishes the compressed form and a patch is computed against
// the uncompressed one.
//
// This is the one place that takes the size out of the frame rather than out of a signed
// manifest, and it is a release job rather than a client: the manifest it would consult
// is the file this run is about to write. The client has no such subcommand and no such
// code path.
int Expand(const fs::path& in, const fs::path& out) {
  const auto frame = Slurp(in);
  if (!frame.has_value()) {
    return 1;
  }
  const auto declared = DeclaredFrameSize(*frame);
  if (!declared.has_value()) {
    return Fail("that file does not say how large it expands to");
  }
  PatchError error = PatchError::kNone;
  const auto bytes = DecompressFrame(*frame, *declared, error);
  if (!bytes.has_value()) {
    return Fail(Describe(error));
  }
  FileStoreError store_error = FileStoreError::kNone;
  if (!WriteFileDurably(out, *bytes, NoFlush(), store_error)) {
    return Fail(Describe(store_error));
  }
  return Report(out);
}

int Inspect(const fs::path& path) {
  const auto bytes = Slurp(path);
  if (!bytes.has_value()) {
    return 1;
  }
  const ParsedHeader parsed = ReadArchiveHeader(*bytes, bytes->size());
  if (!parsed.ok()) {
    return Fail(Describe(parsed.error));
  }
  for (const Member& member : parsed.header.members) {
    std::printf("%12llu  %s  %s\n", static_cast<unsigned long long>(member.size),
                ToHex(member.hash).substr(0, 16).c_str(), member.path.c_str());
  }
  std::fprintf(stderr, "%zu members, %llu bytes of content\n", parsed.header.members.size(),
               static_cast<unsigned long long>(parsed.header.content_size));
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const std::vector<std::string> args(argv + 1, argv + argc);
  if (args.empty()) {
    return Fail("usage: sonora_release pack|compress|expand|delta|describe|inspect ...");
  }
  const std::string& command = args[0];
  if (command == "pack" && args.size() == 3) {
    return Pack(args[1], args[2]);
  }
  if (command == "compress" && args.size() == 3) {
    return Compress(args[1], args[2]);
  }
  if (command == "delta" && args.size() == 4) {
    return Delta(args[1], args[2], args[3]);
  }
  if (command == "expand" && args.size() == 3) {
    return Expand(args[1], args[2]);
  }
  if (command == "describe" && args.size() == 2) {
    return Report(args[1]);
  }
  if (command == "inspect" && args.size() == 2) {
    return Inspect(args[1]);
  }
  return Fail("unknown command, or the wrong number of arguments for it");
}
