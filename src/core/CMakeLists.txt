# sonora_core holds everything that must stay testable without a screen, a sound
# card or an operating system: playback state, library model, bridge payloads.
# It must never link a platform library. CI on macOS builds this target alone,
# which is what keeps that rule honest.

configure_file(
  "${CMAKE_CURRENT_LIST_DIR}/version.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/sonora/core/version.h"
  @ONLY)

add_library(sonora_core STATIC
  src/media_session.cpp
  src/playback_state.cpp)
add_library(sonora::core ALIAS sonora_core)

target_include_directories(sonora_core PUBLIC
  "${CMAKE_CURRENT_LIST_DIR}/include"
  "${CMAKE_CURRENT_BINARY_DIR}/generated")

target_compile_features(sonora_core PUBLIC cxx_std_20)
