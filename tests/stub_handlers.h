#pragma once

#include <string>

#include <bridge_generated.h>
#include <sonora/bridge/protocol.h>

namespace sonora::testing {

// Every method of the generated interface, implemented so that calling one
// fails the test rather than compiling into silence.
//
// It exists because of what happened when week 6 added ten methods to the
// player capability: the application compiled, and the two test files that
// derive from BridgeHandlers did not. That is the interface doing its job --
// a method added to the schema is not optional -- but it is doing it to the
// wrong audience. A test about capability negotiation has no opinion on
// player.seek, and making it write an empty override for one is how a test
// file turns into a wall of boilerplate that nobody reads.
//
// So the enforcement is split. The application still derives from
// BridgeHandlers directly and still has to implement everything, which is the
// property worth having. A test derives from this instead and overrides only
// the methods it is about.
//
// The defaults throw rather than returning a default-constructed result. A
// test that reaches a method it did not mean to reach then says so, instead of
// asserting against a zero that came from nowhere.
class StubHandlers : public bridge::BridgeHandlers {
 public:
#define SONORA_STUB_METHOD(Name)                                                    \
  bridge::Name##Result Name(const bridge::Name##Params& params) override {          \
    (void)params;                                                                   \
    throw bridge::BridgeError(bridge::ErrorCode::kInternalError,                    \
                              std::string(#Name) + " is not stubbed in this test"); \
  }

  SONORA_STUB_METHOD(ShellGetVersion)
  SONORA_STUB_METHOD(ShellEcho)
  SONORA_STUB_METHOD(ShellGetCapabilities)

  SONORA_STUB_METHOD(PlayerGetState)
  SONORA_STUB_METHOD(PlayerEnqueue)
  SONORA_STUB_METHOD(PlayerGetQueue)
  SONORA_STUB_METHOD(PlayerClearQueue)
  SONORA_STUB_METHOD(PlayerPlay)
  SONORA_STUB_METHOD(PlayerPause)
  SONORA_STUB_METHOD(PlayerStop)
  SONORA_STUB_METHOD(PlayerNext)
  SONORA_STUB_METHOD(PlayerPrevious)
  SONORA_STUB_METHOD(PlayerJumpTo)
  SONORA_STUB_METHOD(PlayerSeek)
  SONORA_STUB_METHOD(PlayerSetVolume)

  SONORA_STUB_METHOD(LibraryGetStatus)
  SONORA_STUB_METHOD(LibraryScan)
  SONORA_STUB_METHOD(LibraryListTracks)
  SONORA_STUB_METHOD(LibraryListAlbums)
  SONORA_STUB_METHOD(LibraryListArtists)
  SONORA_STUB_METHOD(LibraryAlbumTracks)
  SONORA_STUB_METHOD(LibraryArtistTracks)
  SONORA_STUB_METHOD(LibrarySearch)

  SONORA_STUB_METHOD(DiagnosticsGetMetrics)

#undef SONORA_STUB_METHOD
};

}  // namespace sonora::testing
