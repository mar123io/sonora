# Rule of the project: no #ifdef _WIN32 outside this directory.
# One interface in iface/, one implementation directory per OS, and CMake picks.
# See docs/adr/0002-platform-abstraction.md.

add_library(sonora_platform STATIC)
add_library(sonora::platform ALIAS sonora_platform)

target_include_directories(sonora_platform PUBLIC
  "${CMAKE_CURRENT_LIST_DIR}/iface/include")

target_link_libraries(sonora_platform PUBLIC sonora::core)

if(WIN32)
  target_sources(sonora_platform PRIVATE
    win/window_win32.cpp
    win/event_loop_win32.cpp
    win/child_window_win32.cpp
    win/paths_win32.cpp
    win/entry_win32.cpp)
  # dwmapi: dark title bar. shcore is not needed, per-monitor v2 awareness comes
  # from the application manifest (see src/shell/sonora.manifest).
  target_link_libraries(sonora_platform PRIVATE dwmapi)

elseif(APPLE)
  set(sonora_platform_mac_sources
    mac/window_mac.mm
    mac/event_loop_mac.mm
    mac/child_window_mac.mm
    mac/paths_mac.mm)
  target_sources(sonora_platform PRIVATE ${sonora_platform_mac_sources})
  set_source_files_properties(${sonora_platform_mac_sources}
    PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  target_link_libraries(sonora_platform PRIVATE "-framework Cocoa")

else()
  # Linux is not a supported target yet. The stubs keep the tree building on
  # Linux CI and make the missing backend a runtime error with a clear message
  # instead of a link error nobody can read.
  target_sources(sonora_platform PRIVATE
    linux/window_stub.cpp
    linux/event_loop_stub.cpp
    linux/child_window_stub.cpp
    linux/paths_stub.cpp)
endif()
