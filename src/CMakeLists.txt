add_subdirectory(core)
add_subdirectory(assets)
add_subdirectory(bridge)
# Before platform: the audio device wrapper lives in the platform layer and
# links this target for miniaudio.
add_subdirectory(audio)
add_subdirectory(library)
# The durable store, which is a different kind of thing from the index next to
# it -- see ADR 0008.
add_subdirectory(state)
# The updater's decisions: manifest, signature, archive, patch, journal. Portable
# on purpose -- see src/update/CMakeLists.txt and ADRs 0009 to 0011.
add_subdirectory(update)
add_subdirectory(platform)

# The updater is a separate executable on purpose -- see src/updater/CMakeLists.txt.
# It needs the platform layer and not CEF, so unlike the shell it is built on every
# platform, and on the two that have no updater yet it links the honest stubs and
# reports that there is no update client rather than pretending.
add_subdirectory(updater)

# The shell is the only target that needs CEF, so it is also the only one the
# macOS and Linux jobs skip.
if(SONORA_BUILD_SHELL)
  add_subdirectory(shell)
endif()
