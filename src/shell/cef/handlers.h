#pragma once

#include <bridge_generated.h>
#include <sonora/bridge/capabilities.h>

namespace sonora::library {
class Library;
}  // namespace sonora::library

namespace sonora::shell {

class AgentHost;
class EventChannel;
class LibraryHost;
class PlayerHost;
class ShellMetrics;

// The application's implementation of the generated bridge interface.
//
// There is no registration step and no table to keep in sync: BridgeHandlers is
// pure virtual, so adding a method to schema/bridge.schema.json stops the build
// here until it is implemented. That is the whole reason the interface is
// generated rather than looked up by name at runtime.
class ShellHandlers final : public bridge::BridgeHandlers {
 public:
  // All of these must outlive the handlers. The runtime owns them and builds this
  // object last, which is the only ordering that works.
  //
  // The agent is a pointer and not a reference, and it is the one that may be null: it
  // dispatches back into this object, so it cannot exist until this object does. The runtime
  // constructs the handlers, then the agent, then hands the agent back with SetAgent. A null
  // one is not an error state -- it is what a build with the capability switched off looks
  // like, and the three agent methods answer kUnavailable, which is the same answer the
  // generated dispatch would have given.
  ShellHandlers(const bridge::CapabilityRegistry& capabilities,
                const ShellMetrics& metrics,
                const EventChannel& events,
                PlayerHost& player,
                LibraryHost& library);

  void SetAgent(AgentHost* agent) { agent_ = agent; }

  // shell
  bridge::ShellGetVersionResult ShellGetVersion(
      const bridge::ShellGetVersionParams& params) override;
  bridge::ShellEchoResult ShellEcho(const bridge::ShellEchoParams& params) override;
  bridge::ShellGetCapabilitiesResult ShellGetCapabilities(
      const bridge::ShellGetCapabilitiesParams& params) override;

  // player
  bridge::PlayerGetStateResult PlayerGetState(
      const bridge::PlayerGetStateParams& params) override;
  bridge::PlayerEnqueueResult PlayerEnqueue(const bridge::PlayerEnqueueParams& params) override;
  bridge::PlayerGetQueueResult PlayerGetQueue(
      const bridge::PlayerGetQueueParams& params) override;
  bridge::PlayerClearQueueResult PlayerClearQueue(
      const bridge::PlayerClearQueueParams& params) override;
  bridge::PlayerPlayResult PlayerPlay(const bridge::PlayerPlayParams& params) override;
  bridge::PlayerPauseResult PlayerPause(const bridge::PlayerPauseParams& params) override;
  bridge::PlayerStopResult PlayerStop(const bridge::PlayerStopParams& params) override;
  bridge::PlayerNextResult PlayerNext(const bridge::PlayerNextParams& params) override;
  bridge::PlayerPreviousResult PlayerPrevious(
      const bridge::PlayerPreviousParams& params) override;
  bridge::PlayerJumpToResult PlayerJumpTo(const bridge::PlayerJumpToParams& params) override;
  bridge::PlayerSeekResult PlayerSeek(const bridge::PlayerSeekParams& params) override;
  bridge::PlayerSetVolumeResult PlayerSetVolume(
      const bridge::PlayerSetVolumeParams& params) override;

  // library
  bridge::LibraryGetStatusResult LibraryGetStatus(
      const bridge::LibraryGetStatusParams& params) override;
  bridge::LibraryScanResult LibraryScan(const bridge::LibraryScanParams& params) override;
  bridge::LibraryListTracksResult LibraryListTracks(
      const bridge::LibraryListTracksParams& params) override;
  bridge::LibraryListAlbumsResult LibraryListAlbums(
      const bridge::LibraryListAlbumsParams& params) override;
  bridge::LibraryListArtistsResult LibraryListArtists(
      const bridge::LibraryListArtistsParams& params) override;
  bridge::LibraryAlbumTracksResult LibraryAlbumTracks(
      const bridge::LibraryAlbumTracksParams& params) override;
  bridge::LibraryArtistTracksResult LibraryArtistTracks(
      const bridge::LibraryArtistTracksParams& params) override;
  bridge::LibrarySearchResult LibrarySearch(const bridge::LibrarySearchParams& params) override;

  // diagnostics
  bridge::DiagnosticsGetMetricsResult DiagnosticsGetMetrics(
      const bridge::DiagnosticsGetMetricsParams& params) override;

  // agent -- see ADR 0016
  bridge::AgentDescribeToolsResult AgentDescribeTools(
      const bridge::AgentDescribeToolsParams& params) override;
  bridge::AgentInterpretResult AgentInterpret(
      const bridge::AgentInterpretParams& params) override;
  bridge::AgentResolveResult AgentResolve(const bridge::AgentResolveParams& params) override;

 private:
  // Throws kUnavailable when the index could not be opened, which is the same
  // answer the transport gives on a machine with no sound card.
  [[nodiscard]] library::Library& RequireIndex();

  // Throws kUnavailable when there is no agent, which is what a build without the capability
  // looks like from here.
  [[nodiscard]] AgentHost& RequireAgent();

  const bridge::CapabilityRegistry& capabilities_;
  const ShellMetrics& metrics_;
  const EventChannel& events_;
  PlayerHost& player_;
  LibraryHost& library_;
  AgentHost* agent_ = nullptr;
};

}  // namespace sonora::shell
