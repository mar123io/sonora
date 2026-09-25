# The audio core: ring buffer, decoders, and the engine that feeds the device.
#
# No CEF, no window, no operating system. The one thing in here that touches a
# platform at all is miniaudio's device layer, and that lives in
# src/platform/audio/ -- this target uses only miniaudio's decoding half, which
# is arithmetic on bytes.
#
# The payoff is the same one src/assets/ and src/bridge/ bought: the macOS and
# Linux CI jobs run these tests, so a lock-free buffer with the memory ordering
# wrong does not have to wait for someone on Windows to hear it.

include(ExternalIncludes)

find_package(Threads REQUIRED)
find_path(SONORA_MINIAUDIO_INCLUDE_DIR "miniaudio.h" REQUIRED)

add_library(sonora_audio STATIC
  src/codecs.cpp
  src/decode_thread.cpp
  src/miniaudio_impl.cpp
  src/player.cpp
  src/ring_buffer.cpp
  src/track_stream.cpp)
add_library(sonora::audio ALIAS sonora_audio)

target_include_directories(sonora_audio PUBLIC
  "${CMAKE_CURRENT_LIST_DIR}/include")

# PUBLIC because src/platform/audio/ includes miniaudio.h too, and both halves
# have to see the same header.
sonora_target_external_include(sonora_audio PUBLIC "${SONORA_MINIAUDIO_INCLUDE_DIR}")

# These change what the header declares, so they are PUBLIC and set in exactly
# one place. See the comment at the top of src/miniaudio_impl.cpp.
#
# Left switched on: decoding (wav, flac, mp3) and device I/O. Switched off:
# everything that would compile a feature this project builds itself.
target_compile_definitions(sonora_audio PUBLIC
  MA_NO_ENCODING          # nothing here writes audio files
  MA_NO_GENERATION        # no test-tone generators; the tests make their own
  MA_NO_NODE_GRAPH        # the mixing graph is ours, in engine.cpp
  MA_NO_ENGINE            # ...and so is the engine on top of it
  MA_NO_RESOURCE_MANAGER) # ...and the caching, when week 7 needs any

target_link_libraries(sonora_audio PUBLIC
  sonora::core
  Threads::Threads)

if(UNIX)
  # miniaudio loads ALSA and PulseAudio at runtime rather than linking them, so
  # there is no development package to install -- but it does need dlopen, and
  # the decoders need the maths library.
  target_link_libraries(sonora_audio PUBLIC ${CMAKE_DL_LIBS} m)
endif()

target_compile_features(sonora_audio PUBLIC cxx_std_20)
