#include "sonora/bench/measure.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

namespace sonora::bench {
namespace {

using Json = nlohmann::json;

const Summary* Find(std::span<const Summary> summaries, std::string_view name) {
  for (const Summary& summary : summaries) {
    if (summary.name == name) {
      return &summary;
    }
  }
  return nullptr;
}

std::string Fixed(double value, int decimals) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
  return buffer;
}

std::string Percent(double fraction) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%+.1f%%", fraction * 100.0);
  return buffer;
}

// The same number without the sign, for a quantity that has no direction: an uncertainty,
// or a threshold. The signed form reads as a change, and "the medians are known to +4.0%"
// is a sentence that makes a reader stop and work out what it means.
std::string Magnitude(double fraction) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.1f%%", std::abs(fraction) * 100.0);
  return buffer;
}

}  // namespace

double Median(std::span<const double> values) {
  if (values.empty()) {
    return 0;
  }
  std::vector<double> sorted(values.begin(), values.end());
  std::sort(sorted.begin(), sorted.end());
  const std::size_t middle = sorted.size() / 2;
  if (sorted.size() % 2 == 1) {
    return sorted[middle];
  }
  return (sorted[middle - 1] + sorted[middle]) / 2.0;
}

Summary Summarise(const Samples& samples) {
  Summary summary;
  summary.name = samples.name;
  summary.unit = samples.unit;
  summary.lower_is_better = samples.lower_is_better;
  summary.samples = samples.values;
  if (summary.samples.size() > kMaxSamples) {
    summary.samples.erase(summary.samples.begin(),
                          summary.samples.end() - static_cast<std::ptrdiff_t>(kMaxSamples));
  }
  summary.repetitions = static_cast<int>(summary.samples.size());
  if (summary.samples.empty()) {
    return summary;
  }

  summary.median = Median(summary.samples);
  summary.minimum = *std::min_element(summary.samples.begin(), summary.samples.end());
  summary.maximum = *std::max_element(summary.samples.begin(), summary.samples.end());

  std::vector<double> deviations;
  deviations.reserve(summary.samples.size());
  for (const double value : summary.samples) {
    deviations.push_back(std::abs(value - summary.median));
  }
  summary.mad = Median(deviations);
  // A median of zero happens for a metric that is a count rather than a duration, and it
  // is not noisy -- it is exact. Reporting spread 0 rather than dividing by zero is the
  // honest answer and it is also what lets a deterministic metric be gated tightly.
  summary.spread = summary.median > 0 ? summary.mad / summary.median : 0.0;
  summary.uncertainty =
      summary.spread * kMedianErrorFactor / std::sqrt(static_cast<double>(summary.repetitions));
  return summary;
}

bool Merge(Summary& into, const Summary& addition) {
  if (addition.samples.empty()) {
    return false;
  }
  if (!into.samples.empty() &&
      (into.unit != addition.unit || into.lower_is_better != addition.lower_is_better)) {
    // Two runs that disagree about the unit are not two runs of the same benchmark, and
    // averaging them would produce a number with no meaning and no complaint.
    return false;
  }

  Samples merged;
  merged.name = into.samples.empty() ? addition.name : into.name;
  merged.unit = addition.unit;
  merged.lower_is_better = addition.lower_is_better;
  merged.values = into.samples;
  merged.values.insert(merged.values.end(), addition.samples.begin(), addition.samples.end());
  into = Summarise(merged);
  return true;
}

void MergeAll(std::vector<Summary>& baseline, std::span<const Summary> addition) {
  for (const Summary& now : addition) {
    bool merged = false;
    for (Summary& before : baseline) {
      if (before.name == now.name) {
        if (!Merge(before, now)) {
          // A unit that changed: the old samples describe a different measurement, so the
          // honest move is to start again rather than to blend them.
          before = now;
        }
        merged = true;
        break;
      }
    }
    if (!merged) {
      baseline.push_back(now);
    }
  }
  std::sort(baseline.begin(), baseline.end(),
            [](const Summary& a, const Summary& b) { return a.name < b.name; });
}

std::string_view Describe(Verdict verdict) {
  switch (verdict) {
    case Verdict::kUnchanged:
      return "unchanged";
    case Verdict::kImproved:
      return "improved";
    case Verdict::kRegressed:
      return "REGRESSED";
    case Verdict::kTooNoisy:
      return "too noisy to gate";
    case Verdict::kNoBaseline:
      return "no baseline";
    case Verdict::kDisappeared:
      return "NOT MEASURED";
  }
  return "unknown";
}

bool IsFailure(Verdict verdict) {
  return verdict == Verdict::kRegressed || verdict == Verdict::kDisappeared;
}

std::vector<Comparison> Compare(std::span<const Summary> baseline,
                                std::span<const Summary> current,
                                const GateOptions& options) {
  std::vector<Comparison> comparisons;

  for (const Summary& now : current) {
    Comparison comparison;
    comparison.name = now.name;
    comparison.unit = now.unit;
    comparison.current_median = now.median;

    if (now.repetitions == 0) {
      // Measured zero times is not measured. Treated as absent rather than as zero
      // milliseconds, which is the value that would otherwise look like a spectacular
      // improvement.
      comparison.verdict = Verdict::kDisappeared;
      comparison.note = "the benchmark ran no repetitions";
      comparisons.push_back(std::move(comparison));
      continue;
    }

    const Summary* before = Find(baseline, now.name);
    if (before == nullptr || before->repetitions == 0) {
      comparison.verdict = Verdict::kNoBaseline;
      comparison.note = "recorded for the first time";
      comparisons.push_back(std::move(comparison));
      continue;
    }
    comparison.baseline_median = before->median;

    if (before->unit != now.unit) {
      // Two numbers in different units are not comparable, and quietly comparing them is
      // how a gate says a metric improved by a factor of a thousand.
      comparison.verdict = Verdict::kDisappeared;
      comparison.note = "the unit changed: " + before->unit + " -> " + now.unit;
      comparisons.push_back(std::move(comparison));
      continue;
    }

    comparison.noise = before->uncertainty + now.uncertainty;
    if (before->median > 0) {
      comparison.change = now.median / before->median - 1.0;
      if (!now.lower_is_better) {
        comparison.change = -comparison.change;
      }
    }

    // The order of these three tests is the design. Noise is asked about before the
    // change, so that a noisy metric is never called a regression; and the absolute floor
    // is asked about before the threshold, so that a benchmark measured in single
    // milliseconds is not gated on a percentage of nothing.
    if (comparison.noise > options.threshold * options.noise_budget) {
      comparison.verdict = Verdict::kTooNoisy;
      comparison.note = "the medians are known to " + Magnitude(comparison.noise) +
                        " against a " + Magnitude(options.threshold) + " gate; measure longer";
    } else if (std::abs(now.median - before->median) < options.minimum_absolute) {
      comparison.verdict = Verdict::kUnchanged;
      comparison.note = "below the absolute floor";
    } else if (comparison.change > options.threshold) {
      comparison.verdict = Verdict::kRegressed;
    } else if (comparison.change < -options.threshold) {
      comparison.verdict = Verdict::kImproved;
    } else {
      comparison.verdict = Verdict::kUnchanged;
    }
    comparisons.push_back(std::move(comparison));
  }

  // And the other direction: anything the baseline knows about that this run did not
  // measure. The easiest way to make a performance gate green is to delete the benchmark.
  for (const Summary& before : baseline) {
    if (Find(current, before.name) != nullptr) {
      continue;
    }
    Comparison comparison;
    comparison.name = before.name;
    comparison.unit = before.unit;
    comparison.baseline_median = before.median;
    comparison.verdict = Verdict::kDisappeared;
    comparison.note = "in the baseline and not measured";
    comparisons.push_back(std::move(comparison));
  }

  std::sort(comparisons.begin(), comparisons.end(),
            [](const Comparison& a, const Comparison& b) { return a.name < b.name; });
  return comparisons;
}

bool AnyFailure(std::span<const Comparison> comparisons) {
  return std::any_of(comparisons.begin(), comparisons.end(),
                     [](const Comparison& c) { return IsFailure(c.verdict); });
}

std::string EncodeBaseline(std::span<const Summary> summaries) {
  Json root;
  root["schema"] = kBaselineSchema;
  Json metrics = Json::array();
  for (const Summary& summary : summaries) {
    Json entry;
    entry["name"] = summary.name;
    entry["unit"] = summary.unit;
    entry["repetitions"] = summary.repetitions;
    entry["median"] = summary.median;
    entry["minimum"] = summary.minimum;
    entry["maximum"] = summary.maximum;
    entry["mad"] = summary.mad;
    entry["spread"] = summary.spread;
    entry["lower_is_better"] = summary.lower_is_better;
    // The samples themselves, so that the next run can append to them and the spread this
    // file reports can be over more than one run. Without them a baseline records the
    // jitter of one process and calls it the noise -- see ADR 0013.
    entry["samples"] = summary.samples;
    metrics.push_back(std::move(entry));
  }
  root["metrics"] = std::move(metrics);
  // Sorted by name in the file as well as in a comparison, so that adding a metric changes
  // one line of the diff rather than reordering the file.
  std::sort(root["metrics"].begin(), root["metrics"].end(), [](const Json& a, const Json& b) {
    return a["name"].get<std::string>() < b["name"].get<std::string>();
  });
  return root.dump(2) + "\n";
}

std::string_view Describe(BaselineError error) {
  switch (error) {
    case BaselineError::kNone:
      return "no error";
    case BaselineError::kTooLarge:
      return "larger than a baseline can be";
    case BaselineError::kNotJson:
      return "not valid JSON";
    case BaselineError::kUnsupportedSchema:
      return "a baseline schema this version does not understand";
    case BaselineError::kBadMetric:
      return "a metric that is missing a field or has the wrong type";
    case BaselineError::kDuplicateMetric:
      return "the same metric twice";
    case BaselineError::kTooMany:
      return "more metrics than this version will read";
  }
  return "unknown error";
}

std::optional<std::vector<Summary>> ParseBaseline(std::string_view json, BaselineError& error) {
  error = BaselineError::kNone;
  if (json.empty() || json.size() > kMaxBaselineBytes) {
    error = BaselineError::kTooLarge;
    return std::nullopt;
  }
  const Json root = Json::parse(json, nullptr, false);
  if (root.is_discarded() || !root.is_object()) {
    error = BaselineError::kNotJson;
    return std::nullopt;
  }
  if (!root.contains("schema") || !root["schema"].is_number_integer() ||
      root["schema"].get<int>() != kBaselineSchema) {
    error = BaselineError::kUnsupportedSchema;
    return std::nullopt;
  }
  if (!root.contains("metrics") || !root["metrics"].is_array()) {
    error = BaselineError::kBadMetric;
    return std::nullopt;
  }
  if (root["metrics"].size() > kMaxMetrics) {
    error = BaselineError::kTooMany;
    return std::nullopt;
  }

  std::vector<Summary> summaries;
  std::set<std::string> names;
  for (const Json& entry : root["metrics"]) {
    if (!entry.is_object()) {
      error = BaselineError::kBadMetric;
      return std::nullopt;
    }
    Summary summary;
    const auto text = [&entry, &error](const char* key, std::string& out) {
      if (!entry.contains(key) || !entry[key].is_string()) {
        error = BaselineError::kBadMetric;
        return false;
      }
      out = entry[key].get<std::string>();
      return true;
    };
    const auto number = [&entry, &error](const char* key, double& out) {
      if (!entry.contains(key) || !entry[key].is_number()) {
        error = BaselineError::kBadMetric;
        return false;
      }
      out = entry[key].get<double>();
      // A baseline with an infinity or a NaN in it would make every comparison against it
      // meaningless, and silently: NaN compares false against everything.
      if (!std::isfinite(out) || out < 0) {
        error = BaselineError::kBadMetric;
        return false;
      }
      return true;
    };
    if (!text("name", summary.name) || !text("unit", summary.unit) ||
        !number("median", summary.median) || !number("minimum", summary.minimum) ||
        !number("maximum", summary.maximum) || !number("mad", summary.mad) ||
        !number("spread", summary.spread)) {
      return std::nullopt;
    }
    if (summary.name.empty() || !entry.contains("repetitions") ||
        !entry["repetitions"].is_number_integer()) {
      error = BaselineError::kBadMetric;
      return std::nullopt;
    }
    summary.repetitions = entry["repetitions"].get<int>();
    if (summary.repetitions < 0) {
      error = BaselineError::kBadMetric;
      return std::nullopt;
    }
    if (entry.contains("lower_is_better")) {
      if (!entry["lower_is_better"].is_boolean()) {
        error = BaselineError::kBadMetric;
        return std::nullopt;
      }
      summary.lower_is_better = entry["lower_is_better"].get<bool>();
    }
    if (entry.contains("samples")) {
      if (!entry["samples"].is_array() || entry["samples"].size() > kMaxSamples) {
        error = BaselineError::kBadMetric;
        return std::nullopt;
      }
      for (const Json& value : entry["samples"]) {
        if (!value.is_number()) {
          error = BaselineError::kBadMetric;
          return std::nullopt;
        }
        const double sample = value.get<double>();
        if (!std::isfinite(sample) || sample < 0) {
          error = BaselineError::kBadMetric;
          return std::nullopt;
        }
        summary.samples.push_back(sample);
      }
    }
    if (!names.insert(summary.name).second) {
      error = BaselineError::kDuplicateMetric;
      return std::nullopt;
    }
    summaries.push_back(std::move(summary));
  }
  return summaries;
}

std::string FormatTable(std::span<const Comparison> comparisons) {
  std::size_t name_width = 6;
  for (const Comparison& comparison : comparisons) {
    name_width = std::max(name_width, comparison.name.size());
  }

  std::string out;
  for (const Comparison& comparison : comparisons) {
    std::string line = comparison.name;
    line.append(name_width - comparison.name.size() + 2, ' ');
    line += Fixed(comparison.baseline_median, 3);
    line += " -> ";
    line += Fixed(comparison.current_median, 3);
    line += " " + comparison.unit;
    line += "  " + Percent(comparison.change);
    if (comparison.noise > 0) {
      // The number ADR 0013 is about, on every line and not only on the noisy ones. A row
      // that says "unchanged, -4.3%" is worth something different depending on whether the
      // two medians are known to 1% or to 9%, and a build log that shows only the first
      // half invites exactly the reading this project spent a week arguing against.
      line += " +/- " + Magnitude(comparison.noise);
    }
    line += "  " + std::string(Describe(comparison.verdict));
    if (!comparison.note.empty()) {
      line += "  (" + comparison.note + ")";
    }
    out += line;
    out += '\n';
  }
  return out;
}

}  // namespace sonora::bench
