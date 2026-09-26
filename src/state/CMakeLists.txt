# The durable store: what the user did, as opposed to what their files say.
#
# It is a separate target from sonora_library on purpose and not for tidiness.
# The two have opposite policies -- one is a cache that may be deleted at any
# moment, the other is the only copy of what it holds -- and keeping them apart
# is what stops the second policy from quietly becoming the first. See ADR 0008.
#
# Portable, like the library: no CEF, no window, no platform. The macOS and
# Linux CI jobs build it and run its tests.
find_package(unofficial-sqlite3 CONFIG QUIET)
if(TARGET unofficial::sqlite3::sqlite3)
  set(SONORA_SQLITE3_TARGET unofficial::sqlite3::sqlite3)
else()
  find_package(SQLite3 REQUIRED)
  set(SONORA_SQLITE3_TARGET SQLite::SQLite3)
endif()

add_library(sonora_state STATIC
  src/store.cpp)
add_library(sonora::state ALIAS sonora_state)

target_include_directories(sonora_state PUBLIC
  "${CMAKE_CURRENT_LIST_DIR}/include")

# PRIVATE: nothing above this target sees sqlite3.h. The header it installs
# names sqlite3 only as a forward declaration, which is what keeps this
# interface a seam rather than a pass-through.
target_link_libraries(sonora_state PRIVATE ${SONORA_SQLITE3_TARGET})

target_compile_features(sonora_state PUBLIC cxx_std_20)
