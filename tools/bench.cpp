// sonora_bench: the numbers, and the gate that fails a build when one of them gets worse.
//
//   sonora_bench --repetitions 9 --files 4000
//                --baseline bench/baseline.json --out bench/current.json
//
// What it measures is portable on purpose: the library scan, the update package and the
// patch. Those run identically on a runner and on a laptop, which is the only way a number
// saved in the repository means anything a month later.
//
// What it does *not* measure is written down rather than left implied: time to first frame,
// resident memory at rest and CPU during playback all need the application, a window and a
// sound card, and a benchmark that cannot run in CI is a benchmark that stops being true
// without anybody noticing. Those three are in the roadmap's list and in this week's "to
// come back to", and they are not silently approximated here.
//
// Exit code 1 if any metric regressed or disappeared. See ADR 0013 for what counts --
// and --report-only, for the events where only the second of those two is worth a
// failure.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <sonora/library/library.h>
#include <sonora/library/scanner.h>
#include <sonora/library/tag_reader.h>

#include "sonora/bench/measure.h"
#include "sonora/update/filestore.h"
#include "sonora/update/hash.h"
#include "sonora/update/patch.h"

namespace {

namespace fs = std::filesystem;
using namespace sonora::bench;
using Clock = std::chrono::steady_clock;

struct Options {
  int repetitions = 9;

  // Iterations run and then thrown away before any are recorded.
  //
  // Three, and the number was read off the recorded samples rather than chosen. Every one of
  // the three baseline runs had the same shape -- the first two iterations of
  // update-patch-apply four to five times the cost of the rest, the third about a quarter
  // high, and flat from the fourth:
  //
  //     0.878  0.639  0.277  0.247  0.208  0.201  0.204  0.211  0.199
  //     0.850  1.086  0.267  0.212  0.195  0.211  0.195  0.194  0.194
  //     0.824  1.018  0.243  0.198  0.194  0.201  0.194  0.193  0.196
  //
  // Which is not this project's code: twenty lines that allocate, touch and free a buffer of
  // the same 8,523,776 bytes reproduce the curve exactly, because glibc serves the first
  // large allocation from mmap, returns it with munmap, and only after a couple of rounds
  // raises its own threshold and starts reusing the pages. So the first two samples measured
  // the kernel faulting in 8.5 MiB, at 0.878 ms against a real cost of 0.195 -- six of the
  // twenty-seven samples in the baseline, which is what dragged its median to 0.208 and, far
  // worse, gave its MAD something to be wide about.
  int warmup = 3;

  int files = 4000;
  fs::path baseline;
  fs::path out;
  fs::path workdir;
  double threshold = 0.10;
  double noise_budget = 0.5;
  std::string only;
  bool update_baseline = false;
  bool append = false;

  // Compare, print the table, and exit 0 anyway if the only complaint is a regression.
  //
  // For the events where a red gate is not a decision anybody is about to act on: a push to
  // main, and a tag. The gate's job is to keep a regression out of main, and it does that on
  // the pull request; by the time a tag is pushed the code has been in main for days and the
  // only thing a failure here can do is refuse to build a release.
  //
  // It downgrades a slow duration and nothing else. A benchmark that went missing, and a
  // regression in a metric that was deterministic on both sides, still fail in every mode --
  // see IsMachineIndependentFailure.
  bool report_only = false;

  // How many times each timed body actually runs.
  [[nodiscard]] int iterations() const { return repetitions + warmup; }
};

double MillisecondsSince(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// A valid WAV of a few samples. taglib reads it, the scanner indexes it, and it is 2 KiB
// rather than 40 MiB -- which matters, because a benchmark that is bound by the disk
// measures the disk.
void WriteTinyWav(const fs::path& path, std::uint32_t seed) {
  constexpr int kSampleRate = 44100;
  constexpr int kChannels = 2;
  constexpr int kSamples = 256;
  const std::uint32_t data_bytes = kSamples * kChannels * 2;

  std::vector<std::uint8_t> bytes;
  const auto u32 = [&bytes](std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
      bytes.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF));
    }
  };
  const auto u16 = [&bytes](std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
  };
  const auto tag = [&bytes](const char* four) { bytes.insert(bytes.end(), four, four + 4); };

  tag("RIFF");
  u32(36 + data_bytes);
  tag("WAVE");
  tag("fmt ");
  u32(16);
  u16(1);  // PCM
  u16(kChannels);
  u32(kSampleRate);
  u32(kSampleRate * kChannels * 2);
  u16(kChannels * 2);
  u16(16);
  tag("data");
  u32(data_bytes);
  std::mt19937 engine(seed);
  for (std::uint32_t i = 0; i < data_bytes; ++i) {
    bytes.push_back(static_cast<std::uint8_t>(engine() & 0xFF));
  }

  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
}

// A library that looks like one: albums in folders, a few tracks each.
void BuildLibrary(const fs::path& root, int files) {
  if (fs::exists(root)) {
    return;
  }
  constexpr int kTracksPerAlbum = 12;
  for (int i = 0; i < files; ++i) {
    char name[64];
    std::snprintf(name, sizeof(name), "%02d - track.wav", i % kTracksPerAlbum + 1);
    char album[64];
    std::snprintf(album, sizeof(album), "artist %03d/album %03d", i / 240, i / kTracksPerAlbum);
    WriteTinyWav(root / album / name, static_cast<std::uint32_t>(i));
  }
}

std::vector<std::uint8_t> Noise(std::size_t size, std::uint32_t seed) {
  std::mt19937 engine(seed);
  std::vector<std::uint8_t> bytes(size);
  for (std::uint8_t& byte : bytes) {
    byte = static_cast<std::uint8_t>(engine() & 0xFF);
  }
  return bytes;
}

// ---------------------------------------------------------------------------
// The benchmarks
// ---------------------------------------------------------------------------

Samples ScanCold(const Options& options, const fs::path& music) {
  Samples samples;
  samples.name = "library-scan-cold";
  samples.unit = "ms";
  const fs::path database = options.workdir / "cold.sqlite";
  for (int i = 0; i < options.iterations(); ++i) {
    std::error_code ec;
    fs::remove(database, ec);
    sonora::library::Library library(database);
    sonora::library::Scanner scanner(library, sonora::library::MakeTagLibReader());
    const auto start = Clock::now();
    const auto progress = scanner.Scan(music);
    samples.values.push_back(MillisecondsSince(start));
    if (i == 0) {
      std::fprintf(stderr, "  (cold scan indexed %d of %d files)\n", progress.added,
                   progress.files_seen);
    }
  }
  return samples;
}

// The one that is the startup cost. Nothing changed, so no tag is read and no row is
// written: what is left is the directory walk and whatever the scanner does per file before
// deciding there is nothing to do. That last part is what week 12 went looking at.
Samples ScanRescan(const Options& options, const fs::path& music) {
  const fs::path database = options.workdir / "warm.sqlite";
  std::error_code ec;
  fs::remove(database, ec);
  {
    sonora::library::Library library(database);
    sonora::library::Scanner scanner(library, sonora::library::MakeTagLibReader());
    static_cast<void>(scanner.Scan(music));
  }

  Samples samples;
  samples.name = "library-scan-rescan";
  samples.unit = "ms";
  for (int i = 0; i < options.iterations(); ++i) {
    sonora::library::Library library(database);
    sonora::library::Scanner scanner(library, sonora::library::MakeTagLibReader());
    const auto start = Clock::now();
    const auto progress = scanner.Scan(music);
    samples.values.push_back(MillisecondsSince(start));
    if (i == 0 && progress.files_read != 0) {
      std::fprintf(stderr, "  (warning: a rescan read %d tags; it should read none)\n",
                   progress.files_read);
    }
  }
  return samples;
}

struct DeltaWork {
  std::vector<std::uint8_t> old_archive;
  std::vector<std::uint8_t> new_archive;
  std::vector<std::uint8_t> delta;
};

DeltaWork PrepareDelta(const Options& options) {
  // Two payloads that differ the way two builds of Sonora differ: one large file that does
  // not change and one small one that does.
  const fs::path base = options.workdir / "payload";
  for (int version = 0; version < 2; ++version) {
    const fs::path dir = base / (version == 0 ? "v1" : "v2");
    if (fs::exists(dir)) {
      continue;
    }
    fs::create_directories(dir);
    const auto write = [](const fs::path& path, const std::vector<std::uint8_t>& bytes) {
      std::ofstream out(path, std::ios::binary | std::ios::trunc);
      out.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    };
    write(dir / "libcef.dll", Noise(8 * 1024 * 1024, 1));
    write(dir / "Sonora.exe", Noise(512 * 1024, version == 0 ? 2 : 3));
  }

  sonora::update::FileStoreError store_error = sonora::update::FileStoreError::kNone;
  const fs::path old_spk = options.workdir / "v1.spk";
  const fs::path new_spk = options.workdir / "v2.spk";
  static_cast<void>(sonora::update::PackDirectory(base / "v1", old_spk, store_error));
  static_cast<void>(sonora::update::PackDirectory(base / "v2", new_spk, store_error));

  DeltaWork work;
  work.old_archive = *sonora::update::ReadFile(old_spk, 1ull << 30, store_error);
  work.new_archive = *sonora::update::ReadFile(new_spk, 1ull << 30, store_error);

  sonora::update::PatchError patch_error = sonora::update::PatchError::kNone;
  work.delta = *sonora::update::CreatePatch(work.old_archive, work.new_archive, patch_error);
  return work;
}

// A size, not a duration: the same number every run, so its spread is zero and it can be
// gated as tightly as anybody likes. The contrast with the scan benchmarks is the point of
// ADR 0013 and it is measured rather than asserted.
Samples DeltaSize(const Options& options, const DeltaWork& work) {
  Samples samples;
  samples.name = "update-delta-size";
  samples.unit = "bytes";
  for (int i = 0; i < options.iterations(); ++i) {
    samples.values.push_back(static_cast<double>(work.delta.size()));
  }
  return samples;
}

Samples PatchApply(const Options& options, const DeltaWork& work) {
  Samples samples;
  samples.name = "update-patch-apply";
  samples.unit = "ms";
  for (int i = 0; i < options.iterations(); ++i) {
    sonora::update::PatchError error = sonora::update::PatchError::kNone;
    const auto start = Clock::now();
    const auto rebuilt = sonora::update::ApplyPatch(work.old_archive, work.delta,
                                                    work.new_archive.size(), error);
    samples.values.push_back(MillisecondsSince(start));
    if (!rebuilt.has_value()) {
      std::fprintf(stderr, "  (the patch did not apply: %s)\n",
                   std::string(Describe(error)).c_str());
    }
  }
  return samples;
}

// ---------------------------------------------------------------------------

std::string ReadWholeFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return {};
  }
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

int Usage() {
  std::fprintf(stderr,
               "usage: sonora_bench [--repetitions N] [--warmup N] [--files N]\n"
               "                    [--workdir DIR] [--baseline FILE] [--out FILE]\n"
               "                    [--threshold F] [--noise-budget F] [--only NAME]\n"
               "                    [--update-baseline] [--append] [--report-only]\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  const std::vector<std::string> args(argv + 1, argv + argc);
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& flag = args[i];
    const auto value = [&args, &i](std::string& out) {
      if (i + 1 >= args.size()) {
        return false;
      }
      out = args[++i];
      return true;
    };
    std::string text;
    if (flag == "--repetitions" && value(text)) {
      options.repetitions = std::atoi(text.c_str());
    } else if (flag == "--warmup" && value(text)) {
      options.warmup = std::atoi(text.c_str());
    } else if (flag == "--report-only") {
      options.report_only = true;
    } else if (flag == "--files" && value(text)) {
      options.files = std::atoi(text.c_str());
    } else if (flag == "--workdir" && value(text)) {
      options.workdir = text;
    } else if (flag == "--baseline" && value(text)) {
      options.baseline = text;
    } else if (flag == "--out" && value(text)) {
      options.out = text;
    } else if (flag == "--threshold" && value(text)) {
      options.threshold = std::atof(text.c_str());
    } else if (flag == "--noise-budget" && value(text)) {
      options.noise_budget = std::atof(text.c_str());
    } else if (flag == "--only" && value(text)) {
      options.only = text;
    } else if (flag == "--update-baseline") {
      options.update_baseline = true;
    } else if (flag == "--append") {
      // Adds this run's samples to the baseline instead of replacing it. The recording job
      // in CI calls the suite three times this way, so that the spread the gate consults is
      // over three independent runs rather than over the jitter of one process -- which is
      // what ADR 0013 is mostly about.
      options.append = true;
      options.update_baseline = true;
    } else {
      return Usage();
    }
  }
  if (options.repetitions < 3) {
    // Three is the fewest from which a median and a spread mean anything at all, and the
    // spread is what decides whether a metric may be gated.
    std::fprintf(stderr, "sonora_bench: at least three repetitions\n");
    return 2;
  }
  if (options.warmup < 0) {
    std::fprintf(stderr, "sonora_bench: --warmup cannot be negative\n");
    return 2;
  }
  if (options.workdir.empty()) {
    options.workdir = fs::temp_directory_path() / "sonora-bench";
  }
  fs::create_directories(options.workdir);

  const fs::path music = options.workdir / "music";
  std::fprintf(stderr, "sonora_bench: %d files, %d repetitions after %d discarded, in %s\n",
               options.files, options.repetitions, options.warmup,
               options.workdir.string().c_str());
  BuildLibrary(music, options.files);
  const DeltaWork delta_work = PrepareDelta(options);

  std::vector<Summary> current;
  const auto run = [&](Samples samples) {
    if (!options.only.empty() && samples.name != options.only) {
      return;
    }
    // Dropped here rather than inside each benchmark, so that "the first N are not evidence"
    // is one rule in one place and cannot be true of three benchmarks and forgotten in the
    // fourth. The discarded samples are never summarised and never reach the baseline file.
    const auto discard = static_cast<std::size_t>(options.warmup);
    if (samples.values.size() > discard) {
      samples.values.erase(samples.values.begin(),
                           samples.values.begin() + static_cast<std::ptrdiff_t>(discard));
    }
    const Summary summary = Summarise(samples);
    std::fprintf(stderr, "  %-22s median %10.3f %-6s spread %5.2f%%\n", summary.name.c_str(),
                 summary.median, summary.unit.c_str(), summary.spread * 100.0);
    current.push_back(summary);
  };

  if (options.only.empty() || options.only == "library-scan-cold") {
    run(ScanCold(options, music));
  }
  if (options.only.empty() || options.only == "library-scan-rescan") {
    run(ScanRescan(options, music));
  }
  run(DeltaSize(options, delta_work));
  run(PatchApply(options, delta_work));

  if (!options.out.empty()) {
    fs::create_directories(options.out.parent_path());
    std::ofstream out(options.out, std::ios::trunc);
    out << EncodeBaseline(current);
  }

  if (options.update_baseline) {
    if (options.baseline.empty()) {
      std::fprintf(stderr, "sonora_bench: --update-baseline needs --baseline\n");
      return 2;
    }
    std::vector<Summary> recorded = current;
    if (options.append) {
      const std::string existing = ReadWholeFile(options.baseline);
      if (!existing.empty()) {
        BaselineError append_error = BaselineError::kNone;
        auto before = ParseBaseline(existing, append_error);
        if (!before.has_value()) {
          std::fprintf(stderr, "sonora_bench: cannot append to %s: %s\n",
                       options.baseline.string().c_str(),
                       std::string(Describe(append_error)).c_str());
          return 2;
        }
        MergeAll(*before, current);
        recorded = std::move(*before);
      }
    }
    fs::create_directories(options.baseline.parent_path());
    std::ofstream out(options.baseline, std::ios::trunc);
    out << EncodeBaseline(recorded);
    std::fprintf(stderr, "sonora_bench: baseline %s to %s\n",
                 options.append ? "appended" : "written", options.baseline.string().c_str());
    for (const Summary& summary : recorded) {
      std::fprintf(stderr, "  %-22s %d samples, median %10.3f %-6s spread %5.2f%%\n",
                   summary.name.c_str(), summary.repetitions, summary.median,
                   summary.unit.c_str(), summary.spread * 100.0);
    }
    return 0;
  }

  if (options.baseline.empty()) {
    return 0;
  }

  const std::string text = ReadWholeFile(options.baseline);
  if (text.empty()) {
    std::fprintf(stderr, "sonora_bench: no baseline at %s; nothing to compare against\n",
                 options.baseline.string().c_str());
    return 0;
  }
  BaselineError baseline_error = BaselineError::kNone;
  const auto baseline = ParseBaseline(text, baseline_error);
  if (!baseline.has_value()) {
    std::fprintf(stderr, "sonora_bench: %s: %s\n", options.baseline.string().c_str(),
                 std::string(Describe(baseline_error)).c_str());
    return 2;
  }

  GateOptions gate;
  gate.threshold = options.threshold;
  gate.noise_budget = options.noise_budget;
  const std::vector<Comparison> comparisons = Compare(*baseline, current, gate);
  std::fputs("\n", stderr);
  std::fputs(FormatTable(comparisons).c_str(), stderr);

  if (AnyFailure(comparisons)) {
    std::fprintf(stderr,
                 "\nsonora_bench: a metric regressed or went missing.\n"
                 "If the change is intended, re-run with --update-baseline and commit the\n"
                 "new %s in the same commit as the change that moved it.\n",
                 options.baseline.filename().string().c_str());
    if (options.report_only && !AnyMachineIndependentFailure(comparisons)) {
      std::fputs(
          "sonora_bench: --report-only, so this is a note and not a failure.\n"
          "Every metric the baseline knows about was measured, and what moved is a\n"
          "duration whose value depends on which host ran it. A metric that is the\n"
          "same number every sample would have failed here anyway. See ADR 0015.\n",
          stderr);
      return 0;
    }
    return 1;
  }
  std::fputs("\nsonora_bench: nothing regressed\n", stderr);
  return 0;
}
