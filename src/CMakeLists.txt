add_subdirectory(core)
add_subdirectory(platform)

if(SONORA_BUILD_SHELL)
  add_subdirectory(shell)
endif()
