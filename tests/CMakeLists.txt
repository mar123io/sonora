find_package(Catch2 3 REQUIRED)

add_executable(sonora_tests
  test_playback_state.cpp
  test_asset_store.cpp
  test_bridge.cpp
  test_capabilities.cpp
  test_event_coalescer.cpp
  test_library.cpp
  test_media_session.cpp
  test_player.cpp
  test_ring_buffer.cpp
  test_scanner.cpp
  test_track_stream.cpp)

# stub_handlers.h lives beside the tests, not in a target's include path.
target_include_directories(sonora_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}")

target_link_libraries(sonora_tests PRIVATE
  sonora::core
  sonora::assets
  sonora::audio
  sonora::bridge
  sonora::library
  Catch2::Catch2WithMain)

include(Catch)
catch_discover_tests(sonora_tests)
