add_subdirectory(core)
add_subdirectory(assets)
add_subdirectory(bridge)
# Before platform: the audio device wrapper lives in the platform layer and
# links this target for miniaudio.
add_subdirectory(audio)
add_subdirectory(library)
add_subdirectory(platform)

# The shell is the only target that needs CEF, so it is also the only one the
# macOS and Linux jobs skip.
if(SONORA_BUILD_SHELL)
  add_subdirectory(shell)
endif()
