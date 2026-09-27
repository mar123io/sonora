#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sonora::bench {

// Measurements, and the arithmetic that decides whether one is worse than another.
//
// All of it portable and none of it measuring anything by itself: what a benchmark does is
// its own business, and this is the part that turns repeated numbers into a verdict a CI
// job can act on. That split is the point -- the rule that decides "regression" is the
// piece most likely to be wrong and the piece least likely to be tested, so here it is a
// pure function with tests pointed at it.
//
// See ADR 0013 for the argument, which in one line is: a gate on a metric you cannot
// measure twice is a coin toss, so the harness measures its own noise and refuses to gate
// what it cannot.

// One benchmark, run `values.size()` times.
struct Samples {
  std::string name;
  std::string unit;  // "ms", "bytes", "%": free text, compared for equality only
  std::vector<double> values;
  // Every metric in Sonora today is one where less is better. The field exists so that the
  // day one is not, the comparison does not silently invert its meaning.
  bool lower_is_better = true;
};

// What a baseline records, and what a comparison is made of.
//
// It keeps the samples and not only the numbers derived from them, and that is the fix for
// the mistake ADR 0013 is mostly about: the spread of one run is the jitter inside one
// process, and the noise a gate actually has to survive is the difference between one run
// and the next. Measured on the machine this was written on, the same code reported 80.2 to
// 93.7 ms across six invocations -- an 8% range -- while every single invocation put its own
// spread at about 2%. A gate that trusted the 2% would fail builds at random.
//
// So a baseline is recorded by appending several independent runs into it (Merge below),
// and the spread it then reports is over all of them.
struct Summary {
  std::string name;
  std::string unit;
  // Every sample that went into the numbers below, newest last. Bounded: see kMaxSamples.
  std::vector<double> samples;
  int repetitions = 0;
  double median = 0;
  double minimum = 0;
  double maximum = 0;
  // Median absolute deviation: the median of |x - median|. Not the standard deviation,
  // because one slow run caused by a virus scanner should not decide whether a metric can
  // be gated, and a mean-based spread lets it.
  double mad = 0;
  // mad / median -- the noise as a fraction of the thing measured, for a person reading the
  // table.
  double spread = 0;
  // How well the *median* is known, as a fraction of it -- which is the number the gate
  // consults, and not the one above.
  //
  // The distinction is the second thing ADR 0013 is about. What a comparison compares is two
  // medians, and the uncertainty of a median falls as the square root of the sample count
  // while the spread of the samples does not fall at all. A gate that consulted the spread
  // would refuse a metric no matter how carefully it was measured; this one gets tighter as
  // the measurement gets longer, which is the right incentive.
  //
  //   uncertainty ~= 1.858 * spread / sqrt(n)
  //
  // from the standard error of a median, 1.2533 * sigma / sqrt(n), with sigma estimated as
  // 1.4826 * MAD. Both constants are the usual normal-distribution ones and both are
  // approximations; what matters is that the number shrinks with n and that it is the same
  // formula on both sides of the comparison.
  double uncertainty = 0;
  bool lower_is_better = true;
};

// 1.2533 * 1.4826, the two normal-distribution constants behind the standard error of a
// median estimated from a MAD.
inline constexpr double kMedianErrorFactor = 1.858;

// Median, MAD and the rest. An empty sample set produces an empty summary with zero
// repetitions, which Compare() treats as unmeasured rather than as zero milliseconds.
[[nodiscard]] Summary Summarise(const Samples& samples);

// How many samples a baseline keeps per metric. Older ones are dropped first, so a baseline
// appended to for a year describes the last few runs rather than the whole history -- which
// is what a gate wants, because the machine of two years ago is not the machine running the
// build.
inline constexpr std::size_t kMaxSamples = 64;

// Appends `addition`'s samples to `into`'s and recomputes everything from the result.
//
// This is how a baseline comes to describe more than one run of one process, which is the
// only way its spread means what the gate needs it to mean. Refuses, and leaves `into`
// alone, if the two disagree about what they are measuring.
[[nodiscard]] bool Merge(Summary& into, const Summary& addition);

// The same, metric by metric, adding any metric the baseline has not seen before.
void MergeAll(std::vector<Summary>& baseline, std::span<const Summary> addition);

[[nodiscard]] double Median(std::span<const double> values);

enum class Verdict {
  kUnchanged,    // within the threshold
  kImproved,     // better than the baseline by more than the threshold, and worth saying
  kRegressed,    // worse by more than the threshold: this is the one that fails a build
  kTooNoisy,     // the measurement cannot support a gate this tight; reported, not gated
  kNoBaseline,   // measured for the first time; recorded, not gated
  kDisappeared,  // in the baseline and not measured -- see the note in Compare()
};

[[nodiscard]] std::string_view Describe(Verdict verdict);
[[nodiscard]] bool IsFailure(Verdict verdict);

struct Comparison {
  std::string name;
  std::string unit;
  double baseline_median = 0;
  double current_median = 0;
  // Signed, as a fraction: +0.2 is twenty percent worse when lower is better.
  double change = 0;
  double noise = 0;  // the two uncertainties added
  Verdict verdict = Verdict::kUnchanged;
  std::string note;  // for a person reading a build log, never for a decision
};

struct GateOptions {
  // A change larger than this, in the wrong direction, fails the build. The roadmap asked
  // for 10%.
  double threshold = 0.10;

  // How much of the threshold the measurement's own uncertainty is allowed to be before the
  // metric stops being gated at all. Half: if the two medians are each known to within 4%
  // and the threshold is 10%, a "regression" of 11% is inside what two runs of the same code
  // would produce, and gating on it would fail builds at random.
  //
  // This is the number ADR 0013 is about. It is not a fudge factor -- it is the difference
  // between a gate and a dice roll, and which side of it a metric falls on is a property of
  // the benchmark rather than of the change being tested.
  double noise_budget = 0.5;

  // Below this, a relative change is arithmetic on rounding. A benchmark that takes 2 ms
  // does not get gated at 10% of 2 ms.
  double minimum_absolute = 0.0;
};

// Compares a run against a baseline, metric by metric, matched by name.
//
// A metric in the baseline that was not measured is kDisappeared and fails, which is
// deliberate: the easiest way to make a performance gate green is to delete the benchmark,
// and removing one on purpose should cost a visible commit to the baseline rather than
// nothing at all.
//
// A metric measured with no baseline is kNoBaseline and does not fail, which is how a new
// benchmark arrives without breaking the build that introduces it.
[[nodiscard]] std::vector<Comparison> Compare(std::span<const Summary> baseline,
                                              std::span<const Summary> current,
                                              const GateOptions& options);

[[nodiscard]] bool AnyFailure(std::span<const Comparison> comparisons);

// The baseline file: a JSON object with a schema number and a list of summaries, written
// with a trailing newline so that a diff of it reads like a diff.
inline constexpr int kBaselineSchema = 1;
inline constexpr std::size_t kMaxBaselineBytes = 1024 * 1024;
inline constexpr std::size_t kMaxMetrics = 256;

[[nodiscard]] std::string EncodeBaseline(std::span<const Summary> summaries);

enum class BaselineError {
  kNone,
  kTooLarge,
  kNotJson,
  kUnsupportedSchema,
  kBadMetric,
  kDuplicateMetric,
  kTooMany,
};

[[nodiscard]] std::string_view Describe(BaselineError error);

[[nodiscard]] std::optional<std::vector<Summary>> ParseBaseline(std::string_view json,
                                                                BaselineError& error);

// A table for a build log: one line per metric, aligned, with the verdict last.
[[nodiscard]] std::string FormatTable(std::span<const Comparison> comparisons);

}  // namespace sonora::bench
