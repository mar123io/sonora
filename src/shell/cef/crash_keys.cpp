#include "cef/crash_keys.h"

#include <sonora/core/version.h>

#include <string>

#include "include/cef_crash_util.h"

namespace sonora::shell {
namespace {

// A no-op when crash reporting is off, which is the case whenever
// installer/crash_reporter.cfg is not beside the executable -- a development build that has
// not copied it, or a machine whose owner deleted it to stop dumps leaving (ADR 0014).
//
// Checked rather than assumed: CefSetCrashKeyValue before CefInitialize with no reporting
// configured is harmless, and a caller that had to remember the difference is a caller that
// will forget.
void Set(const char* key, const std::string& value) {
  if (!CefCrashReportingEnabled()) {
    return;
  }
  CefSetCrashKeyValue(key, value);
}

}  // namespace

void SetBuildCrashKeys() {
  Set("sonora_version", core::kVersion);
  // git describe rather than the three numbers: for a release they agree, and for everything
  // else this is the only string that says which commit and whether the tree was dirty.
  Set("sonora_build", core::kGitDescribe);
}

void SetUpdateCrashKey(std::string_view stage) {
  Set("sonora_update_stage", std::string(stage));
}

void SetLibraryCrashKey(int schema_version) {
  Set("sonora_library_schema", std::to_string(schema_version));
}

void SetAudioCrashKey(bool started, std::string_view backend) {
  Set("sonora_audio_started", started ? std::string("yes:") + std::string(backend) : "no");
}

}  // namespace sonora::shell
