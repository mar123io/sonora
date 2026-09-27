# sonora-updater: a separate executable, and that is a design decision rather than a
# packaging one.
#
# Three reasons, in the order they matter:
#
# 1. It has to be able to move the tree the application was loaded from, and on Windows
#    you can rename a running executable but not delete one. So the process that does the
#    moving copies itself to the temporary directory first, and a process cannot copy
#    itself out of a tree it is 150 MiB of.
# 2. It runs when the application is not running -- at its exit, and before its next
#    start -- which is exactly when a thread inside the application cannot.
# 3. Applying a patch peaks around 430 MiB of resident memory (ADR 0010). In a separate
#    process that is a spike that ends; in the application it is a high-water mark that
#    the allocator keeps for the rest of the session.
#
# It links sonora::update for every decision and sonora::platform for the five things
# std::filesystem cannot do. It does not link CEF, and it does not know what a window is.
add_executable(sonora_updater main.cpp)
set_target_properties(sonora_updater PROPERTIES OUTPUT_NAME "sonora-updater")

target_link_libraries(sonora_updater PRIVATE
  sonora::core
  sonora::update
  sonora::platform)

# Where the manifest lives, from the build rather than from an argument -- see the
# comment in main.cpp. A cache variable so that a fork can point at its own channel
# without editing a source file, and so that the value is recorded in the build.
set(SONORA_UPDATE_MANIFEST_URL
  "https://mar123io.github.io/sonora/updates/stable/manifest.json"
  CACHE STRING "Where sonora-updater looks for the release manifest")

# The platform string in the manifest. Not derived from CMAKE_SYSTEM_NAME: that would
# make the name of a release artefact depend on which generator somebody ran, and the
# manifest's platform names are a published contract.
if(WIN32)
  set(SONORA_UPDATE_PLATFORM "win-x64")
elseif(APPLE)
  set(SONORA_UPDATE_PLATFORM "mac-universal")
else()
  set(SONORA_UPDATE_PLATFORM "linux-x64")
endif()

target_compile_definitions(sonora_updater PRIVATE
  SONORA_UPDATE_MANIFEST_URL="${SONORA_UPDATE_MANIFEST_URL}"
  SONORA_UPDATE_PLATFORM="${SONORA_UPDATE_PLATFORM}")

target_compile_features(sonora_updater PRIVATE cxx_std_20)
