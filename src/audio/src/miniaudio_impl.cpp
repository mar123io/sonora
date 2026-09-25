// The one translation unit in the project that compiles miniaudio.
//
// miniaudio is a single header that is both the declarations and, under
// MINIAUDIO_IMPLEMENTATION, about seventy thousand lines of implementation.
// Defining that macro in two places is a link error; defining it in none is a
// link error with the opposite message. It is defined here, once, and nowhere
// else includes this file.
//
// The feature switches are set on the CMake target rather than here, and PUBLIC
// rather than PRIVATE, on purpose: they change the declarations the header
// emits, so a translation unit that includes miniaudio.h with a different set
// disagrees with this one about the size of a struct. That is a crash with no
// diagnostic, and the only defence is that nobody can set them per file.
//
// What this build uses miniaudio for is two unrelated halves of it:
//
//   * decoding, in src/audio/src/codecs.cpp -- pure computation, no operating
//     system, which is why it is allowed in the portable target;
//   * device I/O, in src/platform/audio/ -- the per-OS half, which is the
//     reason the platform layer does not need a WASAPI file, a CoreAudio file
//     and an ALSA file of its own.

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
