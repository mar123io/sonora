add_subdirectory(core)
add_subdirectory(assets)
add_subdirectory(bridge)
add_subdirectory(platform)

# The shell is the only target that needs CEF, so it is also the only one the
# macOS and Linux jobs skip.
if(SONORA_BUILD_SHELL)
  add_subdirectory(shell)
endif()
