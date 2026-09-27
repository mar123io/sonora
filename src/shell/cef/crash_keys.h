#pragma once

#include <string>
#include <string_view>

namespace sonora::shell {

// The five strings attached to every crash report this process produces after they are set.
//
// CefSetCrashKeyValue takes effect for dumps written afterwards, and there is an ordering
// here that is easy to get backwards. CEF starts Crashpad during CefInitialize, so a key
// set before that call goes nowhere -- and nothing is lost by that, because a fault before
// the handler exists produces no dump at all (ADR 0014). So the first three are set
// together, immediately after CefInitialize, from answers that were learnt a few lines
// earlier: the values are gathered where they are known and attached at the first moment
// there is something to attach them to.
//
// The exception is the update stage, which is set again when it changes -- a crash in a
// version that has not yet been confirmed is a different bug from the same crash in one
// that has.
//
// The keys themselves are declared in installer/crash_reporter.cfg.in, which CEF reads -- in
// its generated form, beside the executable -- before any of this runs. A key set here and
// absent there is dropped silently, which is why the two lists are next to each other in
// ADR 0014 with the instruction to change them together.
//
// What is deliberately not here: the library path, any file name, anything identifying the
// machine or the person. A crash report is the most sensitive thing a desktop application
// sends, and this list is short enough that somebody can read it and decide.

// The version and the kind of build. The two strings that have to match a PDB.
void SetBuildCrashKeys();

// Where the updater thinks it is: the journal's stage, or "idle". A crash in a version that
// was installed twenty seconds ago and has not yet been confirmed is a different bug from the
// same crash in a version that has been running for a month.
void SetUpdateCrashKey(std::string_view stage);

// The library index's schema version, as a string. Two schema versions in the wild at once is
// what a migration looks like from the outside, and it has been the answer twice.
void SetLibraryCrashKey(int schema_version);

// Whether the audio device started, and with which backend if it did. "no" is a perfectly
// ordinary state -- a machine with no sound card, or one whose device was taken by something
// exclusive -- and it changes which half of the program a stack trace is about.
void SetAudioCrashKey(bool started, std::string_view backend);

}  // namespace sonora::shell
