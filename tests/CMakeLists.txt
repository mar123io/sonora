find_package(Catch2 3 REQUIRED)

add_executable(sonora_tests
  test_playback_state.cpp
  test_asset_store.cpp
  test_bench_measure.cpp
  test_bridge.cpp
  test_capabilities.cpp
  test_deep_link.cpp
  test_event_coalescer.cpp
  test_library.cpp
  test_media_session.cpp
  test_player.cpp
  test_ring_buffer.cpp
  test_scanner.cpp
  test_state_store.cpp
  test_track_stream.cpp
  test_update_archive.cpp
  test_update_e2e.cpp
  test_update_journal.cpp
  test_update_manifest.cpp
  test_update_patch.cpp
  test_update_rollout.cpp
  test_update_version.cpp
  test_window_placement.cpp)

# stub_handlers.h lives beside the tests, not in a target's include path.
target_include_directories(sonora_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}")

target_link_libraries(sonora_tests PRIVATE
  sonora::core
  sonora::assets
  sonora::audio
  sonora::bench
  sonora::bridge
  sonora::library
  sonora::state
  sonora::update
  Catch2::Catch2WithMain)

# libsodium, for the tests and for nothing else in this file.
#
# test_update_e2e.cpp signs a manifest, and sonora::update deliberately cannot: the
# client verifies and never signs, so there is no SignDetached in the library to reach
# for. The test makes a keypair of its own and signs with libsodium directly -- which is
# also why the release key's private half is not needed and is not in this tree.
#
# The lookup is repeated here rather than reused from src/update/CMakeLists.txt because
# an imported target created by find_package belongs to the directory that called it, and
# borrowing one across directories works until the day it does not.
find_package(unofficial-sodium CONFIG QUIET)
if(TARGET unofficial-sodium::sodium)
  target_link_libraries(sonora_tests PRIVATE unofficial-sodium::sodium)
else()
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(SONORA_TESTS_SODIUM QUIET IMPORTED_TARGET libsodium)
  endif()
  if(TARGET PkgConfig::SONORA_TESTS_SODIUM)
    target_link_libraries(sonora_tests PRIVATE PkgConfig::SONORA_TESTS_SODIUM)
  else()
    message(FATAL_ERROR
      "libsodium not found; tests/test_update_e2e.cpp signs a manifest with it.")
  endif()
endif()

include(Catch)
catch_discover_tests(sonora_tests)
