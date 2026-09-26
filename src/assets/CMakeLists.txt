# The web UI as bytes: the embedded table generated from ui/dist, plus the two
# stores that serve it.
#
# Deliberately independent of CEF. Serving files is not a browser concern, and
# keeping it separate is what lets the macOS and Linux CI jobs run these tests
# without downloading a gigabyte of Chromium.

include(EmbedAssets)
sonora_embed_ui_assets(SONORA_EMBEDDED_ASSETS)

# When the build is building the UI, the embedding step has to run after it.
# When it is not, this is empty and nothing changes. See cmake/WebUI.cmake.
sonora_ui_dependency(SONORA_UI_DEPENDENCY)

add_library(sonora_assets STATIC
  src/asset_store.cpp
  "${SONORA_EMBEDDED_ASSETS}")
add_library(sonora::assets ALIAS sonora_assets)

target_include_directories(sonora_assets PUBLIC
  "${CMAKE_CURRENT_LIST_DIR}/include")

target_compile_features(sonora_assets PUBLIC cxx_std_20)

if(SONORA_UI_DEPENDENCY)
  add_dependencies(sonora_assets ${SONORA_UI_DEPENDENCY})
endif()

# Debug builds serve the UI from disk, so a UI change is a reload rather than a
# C++ rebuild. Release builds have no such path and use the embedded table.
target_compile_definitions(sonora_assets PRIVATE
  $<$<CONFIG:Debug>:SONORA_UI_DIST_DIR="${CMAKE_SOURCE_DIR}/ui/dist">)
