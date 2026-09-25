find_package(Catch2 3 REQUIRED)

add_executable(sonora_tests
  test_playback_state.cpp
  test_asset_store.cpp
  test_bridge.cpp
  test_audio_engine.cpp
  test_capabilities.cpp
  test_event_coalescer.cpp
  test_ring_buffer.cpp)

target_link_libraries(sonora_tests PRIVATE
  sonora::core
  sonora::assets
  sonora::audio
  sonora::bridge
  Catch2::Catch2WithMain)

include(Catch)
catch_discover_tests(sonora_tests)
