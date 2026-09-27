#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

#include "sonora/bench/measure.h"

namespace {

using sonora::bench::AnyFailure;
using sonora::bench::BaselineError;
using sonora::bench::Compare;
using sonora::bench::Comparison;
using sonora::bench::EncodeBaseline;
using sonora::bench::GateOptions;
using sonora::bench::Median;
using sonora::bench::Merge;
using sonora::bench::MergeAll;
using sonora::bench::ParseBaseline;
using sonora::bench::Samples;
using sonora::bench::Summarise;
using sonora::bench::Summary;
using sonora::bench::Verdict;

Summary Make(std::string name, std::vector<double> values, std::string unit = "ms") {
  Samples samples;
  samples.name = std::move(name);
  samples.unit = std::move(unit);
  samples.values = std::move(values);
  return Summarise(samples);
}

// A benchmark that always measures the same thing: zero spread, and therefore gateable as
// tightly as you like.
Summary Exact(std::string name, double value, std::string unit = "bytes") {
  return Make(std::move(name), {value, value, value, value, value}, std::move(unit));
}

// By value, not by reference. Half the cases below call this on the temporary that
// Compare() returns, and a reference into it dangles the moment the expression ends -- a
// warning GCC gives and one worth taking seriously in a file whose whole job is to be
// trusted about what it measured.
Comparison Get(const std::vector<Comparison>& comparisons, std::string_view name) {
  for (const Comparison& comparison : comparisons) {
    if (comparison.name == name) {
      return comparison;
    }
  }
  REQUIRE(false);
  return {};
}

}  // namespace

TEST_CASE("the median is the middle, and does not care about an outlier") {
  CHECK(Median(std::vector<double>{}) == 0);
  CHECK(Median(std::vector<double>{7}) == 7);
  CHECK(Median(std::vector<double>{1, 2, 3}) == 2);
  CHECK(Median(std::vector<double>{3, 1, 2}) == 2);  // sorts first
  CHECK(Median(std::vector<double>{1, 2, 3, 4}) == 2.5);
  // The reason it is the median: one run that took a hundred times as long, because
  // something else on the machine woke up, must not move the number.
  CHECK(Median(std::vector<double>{10, 10, 10, 10, 1000}) == 10);
}

TEST_CASE("a summary of nothing is not a summary of zero") {
  const Summary summary = Make("nothing", {});
  CHECK(summary.repetitions == 0);
  CHECK(summary.median == 0);
  // And the comparison has to know the difference, because zero milliseconds is the value
  // that looks like the best improvement ever recorded.
  const Summary baseline = Make("nothing", {100, 100, 100});
  const std::vector<Summary> before = {baseline};
  const std::vector<Summary> after = {summary};
  const auto comparisons = Compare(before, after, GateOptions{});
  CHECK(Get(comparisons, "nothing").verdict == Verdict::kDisappeared);
  CHECK(AnyFailure(comparisons));
}

TEST_CASE("spread is the noise as a fraction of the thing measured") {
  const Summary steady = Make("steady", {100, 100, 100, 100, 100});
  CHECK(steady.median == 100);
  CHECK(steady.mad == 0);
  CHECK(steady.spread == 0);

  const Summary noisy = Make("noisy", {80, 90, 100, 110, 120});
  CHECK(noisy.median == 100);
  CHECK(noisy.mad == 10);
  CHECK(noisy.spread > 0.09);
  CHECK(noisy.spread < 0.11);
}

TEST_CASE("a quiet metric that got worse is a regression") {
  const std::vector<Summary> before = {Make("scan", {100, 100, 101, 99, 100})};
  const std::vector<Summary> after = {Make("scan", {130, 131, 130, 129, 130})};
  const auto comparisons = Compare(before, after, GateOptions{});
  const Comparison scan = Get(comparisons, "scan");
  CHECK(scan.verdict == Verdict::kRegressed);
  CHECK(scan.change > 0.29);
  CHECK(AnyFailure(comparisons));
}

TEST_CASE("a quiet metric that got better says so") {
  const std::vector<Summary> before = {Make("scan", {100, 100, 100})};
  const std::vector<Summary> after = {Make("scan", {70, 70, 70})};
  const auto comparisons = Compare(before, after, GateOptions{});
  CHECK(Get(comparisons, "scan").verdict == Verdict::kImproved);
  CHECK_FALSE(AnyFailure(comparisons));
}

TEST_CASE("a change inside the threshold is not a change") {
  const std::vector<Summary> before = {Make("scan", {100, 100, 100})};
  for (const double now : {95.0, 100.0, 105.0, 109.0}) {
    const std::vector<Summary> after = {Make("scan", {now, now, now})};
    CHECK(Get(Compare(before, after, GateOptions{}), "scan").verdict == Verdict::kUnchanged);
  }
}

TEST_CASE("a metric noisier than half the threshold is not gated at all") {
  // This is the whole of ADR 0013. The measurement moved by 30%, which is well past the
  // 10% gate -- and the benchmark's own spread is 12%, so two runs of the *same* code
  // differ by more than the gate. Calling that a regression would fail builds at random,
  // and the honest answer is to report the number and refuse to judge it.
  const std::vector<Summary> before = {Make("noisy", {88, 94, 100, 106, 112})};
  const std::vector<Summary> after = {Make("noisy", {118, 124, 130, 136, 142})};
  const auto comparisons = Compare(before, after, GateOptions{});
  const Comparison noisy = Get(comparisons, "noisy");
  CHECK(noisy.verdict == Verdict::kTooNoisy);
  CHECK(noisy.change > 0.29);            // the change is real and reported
  CHECK_FALSE(AnyFailure(comparisons));  // ...and it does not fail the build
  CHECK(noisy.note.find("known to") != std::string::npos);
}

TEST_CASE("the same noisy metric is gated once the threshold is loose enough for it") {
  // The other half of the argument: nothing is ungateable in principle, only ungateable at
  // a given tightness. A 60% gate over a 12% spread is a gate again.
  const std::vector<Summary> before = {Make("noisy", {88, 94, 100, 106, 112})};
  const std::vector<Summary> after = {Make("noisy", {176, 188, 200, 212, 224})};
  GateOptions loose;
  loose.threshold = 0.60;
  const auto comparisons = Compare(before, after, loose);
  CHECK(Get(comparisons, "noisy").verdict == Verdict::kRegressed);
}

TEST_CASE("a deterministic metric can be gated as tightly as you like") {
  // The contrast that makes the rule usable: the size of a delta is the same number every
  // time, so a 1% gate on it is a real gate. Time is not like that and the harness knows.
  const std::vector<Summary> before = {Exact("delta-bytes", 86053)};
  const std::vector<Summary> after = {Exact("delta-bytes", 88000)};
  GateOptions tight;
  tight.threshold = 0.01;
  const auto comparisons = Compare(before, after, tight);
  CHECK(Get(comparisons, "delta-bytes").verdict == Verdict::kRegressed);

  const std::vector<Summary> unchanged = {Exact("delta-bytes", 86053)};
  CHECK(Get(Compare(before, unchanged, tight), "delta-bytes").verdict == Verdict::kUnchanged);
}

TEST_CASE("a new metric is recorded, not gated") {
  const std::vector<Summary> before = {Make("old", {100, 100, 100})};
  const std::vector<Summary> after = {Make("old", {100, 100, 100}), Make("new", {5, 5, 5})};
  const auto comparisons = Compare(before, after, GateOptions{});
  CHECK(Get(comparisons, "new").verdict == Verdict::kNoBaseline);
  CHECK_FALSE(AnyFailure(comparisons));
}

TEST_CASE("deleting a benchmark does not make the gate green") {
  const std::vector<Summary> before = {Make("kept", {100, 100, 100}),
                                       Make("gone", {50, 50, 50})};
  const std::vector<Summary> after = {Make("kept", {100, 100, 100})};
  const auto comparisons = Compare(before, after, GateOptions{});
  CHECK(Get(comparisons, "gone").verdict == Verdict::kDisappeared);
  CHECK(AnyFailure(comparisons));
}

TEST_CASE("a metric whose unit changed is not compared") {
  const std::vector<Summary> before = {Make("thing", {1000, 1000, 1000}, "ms")};
  const std::vector<Summary> after = {Make("thing", {1, 1, 1}, "s")};
  const auto comparisons = Compare(before, after, GateOptions{});
  const Comparison thing = Get(comparisons, "thing");
  // The same duration, and a naive comparison would call it a thousandfold improvement.
  CHECK(thing.verdict == Verdict::kDisappeared);
  CHECK(thing.note.find("unit") != std::string::npos);
}

TEST_CASE("a metric measured in single milliseconds is not gated on a percentage of nothing") {
  const std::vector<Summary> before = {Make("tiny", {2, 2, 2})};
  const std::vector<Summary> after = {Make("tiny", {3, 3, 3})};  // +50%, and one millisecond
  GateOptions options;
  options.minimum_absolute = 5.0;
  CHECK(Get(Compare(before, after, options), "tiny").verdict == Verdict::kUnchanged);
  // Without the floor it is a regression, which is the point of having the floor.
  CHECK(Get(Compare(before, after, GateOptions{}), "tiny").verdict == Verdict::kRegressed);
}

TEST_CASE("a metric where more is better does not have its meaning inverted") {
  Samples samples;
  samples.name = "throughput";
  samples.unit = "files/s";
  samples.lower_is_better = false;
  samples.values = {1000, 1000, 1000};
  const Summary before = Summarise(samples);

  samples.values = {700, 700, 700};
  const Summary after = Summarise(samples);

  const std::vector<Summary> baseline = {before};
  const std::vector<Summary> current = {after};
  const Comparison throughput = Get(Compare(baseline, current, GateOptions{}), "throughput");
  // Thirty percent fewer files per second is thirty percent worse, and the sign of `change`
  // says worse rather than smaller.
  CHECK(throughput.verdict == Verdict::kRegressed);
  CHECK(throughput.change > 0.29);
}

TEST_CASE("a baseline round-trips through the file it is written to") {
  const std::vector<Summary> summaries = {Make("scan", {100, 110, 105}),
                                          Exact("delta-bytes", 86053)};
  BaselineError error = BaselineError::kNone;
  const auto again = ParseBaseline(EncodeBaseline(summaries), error);
  REQUIRE(error == BaselineError::kNone);
  REQUIRE(again.has_value());
  REQUIRE(again->size() == 2);

  // Sorted by name in the file, so adding a metric changes one part of a diff.
  CHECK((*again)[0].name == "delta-bytes");
  CHECK((*again)[1].name == "scan");
  for (const Summary& original : summaries) {
    bool found = false;
    for (const Summary& parsed : *again) {
      if (parsed.name != original.name) {
        continue;
      }
      found = true;
      CHECK(parsed.unit == original.unit);
      CHECK(parsed.repetitions == original.repetitions);
      CHECK(parsed.median == original.median);
      CHECK(parsed.mad == original.mad);
      CHECK(parsed.spread == original.spread);
      CHECK(parsed.lower_is_better == original.lower_is_better);
    }
    CHECK(found);
  }
}

TEST_CASE("a baseline this version cannot read is an error, not an empty baseline") {
  BaselineError error = BaselineError::kNone;
  CHECK_FALSE(ParseBaseline("", error).has_value());
  CHECK(error == BaselineError::kTooLarge);
  CHECK_FALSE(ParseBaseline("{", error).has_value());
  CHECK(error == BaselineError::kNotJson);
  CHECK_FALSE(ParseBaseline(R"({"schema": 2, "metrics": []})", error).has_value());
  CHECK(error == BaselineError::kUnsupportedSchema);
  CHECK_FALSE(ParseBaseline(R"({"metrics": []})", error).has_value());
  CHECK(error == BaselineError::kUnsupportedSchema);
  CHECK_FALSE(ParseBaseline(R"({"schema": 1})", error).has_value());
  CHECK(error == BaselineError::kBadMetric);

  // An empty baseline is legitimate: it is what the first run compares against.
  const auto empty = ParseBaseline(R"({"schema": 1, "metrics": []})", error);
  CHECK(error == BaselineError::kNone);
  REQUIRE(empty.has_value());
  CHECK(empty->empty());
}

TEST_CASE("a baseline with a number that is not one is refused") {
  BaselineError error = BaselineError::kNone;
  const auto with = [&error](std::string_view median) {
    const std::string json = std::string(R"({"schema": 1, "metrics": [{"name": "a",
      "unit": "ms", "repetitions": 3, "median": )") +
                             std::string(median) +
                             R"(, "minimum": 1, "maximum": 1, "mad": 0, "spread": 0}]})";
    return ParseBaseline(json, error).has_value();
  };
  CHECK(with("1.0"));
  // A NaN in a baseline would make every comparison against it meaningless and silently,
  // because NaN compares false against everything -- including the threshold.
  CHECK_FALSE(with("null"));
  CHECK_FALSE(with("\"1.0\""));
  CHECK_FALSE(with("-1.0"));

  CHECK_FALSE(ParseBaseline(R"({"schema": 1, "metrics": [
      {"name": "a", "unit": "ms", "repetitions": 1, "median": 1, "minimum": 1,
       "maximum": 1, "mad": 0, "spread": 0},
      {"name": "a", "unit": "ms", "repetitions": 1, "median": 2, "minimum": 2,
       "maximum": 2, "mad": 0, "spread": 0}]})",
                            error)
                  .has_value());
  CHECK(error == BaselineError::kDuplicateMetric);
}

TEST_CASE("the table is one line per metric and mentions every verdict it shows") {
  const std::vector<Summary> before = {Make("a", {100, 100, 100}), Make("b", {50, 50, 50})};
  const std::vector<Summary> after = {Make("a", {130, 130, 130}), Make("c", {1, 1, 1})};
  const std::string table = sonora::bench::FormatTable(Compare(before, after, GateOptions{}));
  CHECK(std::count(table.begin(), table.end(), '\n') == 3);
  CHECK(table.find("REGRESSED") != std::string::npos);
  CHECK(table.find("NOT MEASURED") != std::string::npos);
  CHECK(table.find("no baseline") != std::string::npos);
}

TEST_CASE("a baseline keeps its samples, so a later run can append to them") {
  const std::vector<Summary> first = {Make("scan", {100, 102, 98})};
  BaselineError error = BaselineError::kNone;
  const auto read_back = ParseBaseline(EncodeBaseline(first), error);
  REQUIRE(read_back.has_value());
  REQUIRE(read_back->size() == 1);
  CHECK((*read_back)[0].samples == std::vector<double>{100, 102, 98});
}

TEST_CASE("appending runs is what makes an uncertainty honest") {
  // The shape of the numbers that caused this: three runs, each steady inside itself, eight
  // percent apart from each other. A baseline built from one of them claims to know its
  // median perfectly.
  const Summary naive = Make("scan", {100, 100, 101});
  CHECK(naive.uncertainty < 0.005);

  Summary merged = naive;
  REQUIRE(Merge(merged, Make("scan", {108, 108, 109})));
  REQUIRE(Merge(merged, Make("scan", {93, 93, 94})));
  CHECK(merged.repetitions == 9);
  CHECK(merged.uncertainty > 0.02);  // ...and this one admits what it is.

  // Which is the difference between a gate and a dice roll. At a two percent threshold the
  // naive baseline is happy to call a seven percent move a regression; the honest one says
  // it does not know its own median that well, and declines.
  const std::vector<Summary> current = {Make("scan", {107, 107, 108})};
  GateOptions tight;
  tight.threshold = 0.02;
  const std::vector<Summary> one_run = {naive};
  const std::vector<Summary> three_runs = {merged};
  CHECK(Get(Compare(one_run, current, tight), "scan").verdict == Verdict::kRegressed);
  CHECK(Get(Compare(three_runs, current, tight), "scan").verdict == Verdict::kTooNoisy);
}

TEST_CASE("measuring longer makes a gate tighter, and that is the incentive") {
  // The same noise, more samples: the spread does not move and the uncertainty falls, which
  // is why the gate consults the second one.
  Samples few;
  few.name = "scan";
  few.unit = "ms";
  few.values = {90, 95, 100, 105, 110};
  Samples many = few;
  for (int i = 0; i < 4; ++i) {
    many.values.insert(many.values.end(), few.values.begin(), few.values.end());
  }
  const Summary with_five = Summarise(few);
  const Summary with_twenty_five = Summarise(many);
  CHECK(with_five.spread == with_twenty_five.spread);
  CHECK(with_twenty_five.uncertainty < with_five.uncertainty / 2.0);
}

TEST_CASE("merging refuses to blend two different measurements") {
  Summary milliseconds = Make("thing", {1000, 1000, 1000}, "ms");
  CHECK_FALSE(Merge(milliseconds, Make("thing", {1, 1, 1}, "s")));
  // Unchanged: a failed merge must not leave half of the old samples and half of the new.
  CHECK(milliseconds.median == 1000);
  CHECK(milliseconds.unit == "ms");
  CHECK_FALSE(Merge(milliseconds, Make("thing", {})));
  CHECK(milliseconds.repetitions == 3);
}

TEST_CASE("merging a suite adds what is new and appends to what is not") {
  std::vector<Summary> baseline = {Make("a", {10, 10, 10}), Make("b", {20, 20, 20})};
  const std::vector<Summary> addition = {Make("b", {22, 22, 22}), Make("c", {30, 30, 30})};
  MergeAll(baseline, addition);
  REQUIRE(baseline.size() == 3);
  CHECK(baseline[0].name == "a");  // sorted, and untouched
  CHECK(baseline[0].repetitions == 3);
  CHECK(baseline[1].name == "b");
  CHECK(baseline[1].repetitions == 6);  // appended
  CHECK(baseline[2].name == "c");
  CHECK(baseline[2].repetitions == 3);  // added
}

TEST_CASE("a baseline appended to forever keeps the recent runs and not the history") {
  Summary summary = Make("scan", {1, 1, 1});
  for (int i = 0; i < 40; ++i) {
    REQUIRE(Merge(summary, Make("scan", {100, 100, 100})));
  }
  CHECK(summary.samples.size() == sonora::bench::kMaxSamples);
  // The three ones from the very first run are long gone, so the median is the machine of
  // now rather than an average of two years.
  CHECK(summary.median == 100);
}

TEST_CASE("a baseline with more samples than it may hold is refused") {
  std::string samples = "[";
  for (std::size_t i = 0; i <= sonora::bench::kMaxSamples; ++i) {
    samples += (i == 0 ? "1" : ",1");
  }
  samples += "]";
  const std::string json = std::string(R"({"schema": 1, "metrics": [{"name": "a",
    "unit": "ms", "repetitions": 3, "median": 1, "minimum": 1, "maximum": 1, "mad": 0,
    "spread": 0, "samples": )") +
                           samples + "}]}";
  BaselineError error = BaselineError::kNone;
  CHECK_FALSE(ParseBaseline(json, error).has_value());
  CHECK(error == BaselineError::kBadMetric);
}
