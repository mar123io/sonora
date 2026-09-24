# The native<->web protocol: envelope, error vocabulary, generated types and
# dispatch.
#
# Deliberately free of CEF. The transport moves two strings, so everything that
# decides what a request MEANS is testable without a browser -- which is why the
# macOS and Linux CI jobs run these tests and the Windows-only shell does not
# have to be the first place a protocol bug shows up.

include(GenerateBridge)
sonora_generate_bridge(SONORA_BRIDGE_HEADER SONORA_BRIDGE_SOURCE)

find_package(nlohmann_json CONFIG REQUIRED)

add_library(sonora_bridge STATIC
  src/capabilities.cpp
  src/event_coalescer.cpp
  src/protocol.cpp
  "${SONORA_BRIDGE_SOURCE}")
add_library(sonora::bridge ALIAS sonora_bridge)

target_include_directories(sonora_bridge PUBLIC
  "${CMAKE_CURRENT_LIST_DIR}/include"
  "${CMAKE_CURRENT_BINARY_DIR}/generated")

target_link_libraries(sonora_bridge PUBLIC nlohmann_json::nlohmann_json)
target_compile_features(sonora_bridge PUBLIC cxx_std_20)

# The generated header is listed as a source so the custom command runs before
# anything that includes it, not just before protocol.cpp.
target_sources(sonora_bridge PRIVATE "${SONORA_BRIDGE_HEADER}")
