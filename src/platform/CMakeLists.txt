# Rule of the project: no #ifdef _WIN32 outside this directory.
# One interface in iface/, one implementation directory per OS, and CMake picks.
# See docs/adr/0002-platform-abstraction.md.

add_library(sonora_platform STATIC)
add_library(sonora::platform ALIAS sonora_platform)

target_include_directories(sonora_platform PUBLIC
  "${CMAKE_CURRENT_LIST_DIR}/iface/include")

# sonora::audio for the miniaudio header and its one compiled translation unit.
# The audio device is the exception to the per-OS rule above -- see the comment
# at the top of audio/audio_device_miniaudio.cpp.
target_link_libraries(sonora_platform PUBLIC sonora::core sonora::audio)

target_sources(sonora_platform PRIVATE audio/audio_device_miniaudio.cpp)

if(WIN32)
  target_sources(sonora_platform PRIVATE
    win/window_win32.cpp
    win/event_loop_win32.cpp
    win/child_window_win32.cpp
    win/paths_win32.cpp
    win/arguments_win32.cpp
    win/media_integration_win32.cpp
    win/displays_win32.cpp
    win/shell_integration_win32.cpp
    win/single_instance_win32.cpp
    win/url_scheme_win32.cpp
    win/entry_win32.cpp)
  # dwmapi: dark title bar. shcore is not needed, per-monitor v2 awareness comes
  # from the application manifest (see src/shell/sonora.manifest).
  # shell32: CommandLineToArgvW.
  #
  # WindowsApp: the umbrella import library for the WinRT APIs a desktop
  # application may call. SystemMediaTransportControls lives there, and so does
  # the interop interface that hands it a session for an HWND.
  #
  # propsys and shlwapi: the window's property store, which is where an
  # unpackaged application tells the shell its name and icon. See
  # ConfigureShellIdentity in win/window_win32.cpp.
  # comctl32: SetWindowSubclass, which is how the shell integration sees the
  # application window's messages without window_win32.cpp knowing it exists.
  # advapi32: the registry, for the sonora:// scheme.
  target_link_libraries(sonora_platform PRIVATE
    dwmapi shell32 propsys shlwapi comctl32 advapi32 WindowsApp)

elseif(APPLE)
  set(sonora_platform_mac_sources
    mac/window_mac.mm
    mac/event_loop_mac.mm
    mac/child_window_mac.mm
    mac/paths_mac.mm
    mac/arguments_mac.mm
    mac/media_integration_mac.mm)
  target_sources(sonora_platform PRIVATE ${sonora_platform_mac_sources})
  # No status-bar item, no Dock menu and no Launch Services registration yet:
  # those are a week of their own, and shared/ says so out loud rather than
  # leaving three undefined symbols.
  target_sources(sonora_platform PRIVATE
    shared/displays_none.cpp
    shared/shell_integration_none.cpp
    shared/single_instance_none.cpp
    shared/url_scheme_none.cpp)
  set_source_files_properties(${sonora_platform_mac_sources}
    PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  # MediaPlayer: MPNowPlayingInfoCenter and MPRemoteCommandCenter. Compiled by
  # CI and never run -- see the comment at the top of media_integration_mac.mm.
  target_link_libraries(sonora_platform PRIVATE "-framework Cocoa" "-framework MediaPlayer")

else()
  # Linux is not a supported target yet. The stubs keep the tree building on
  # Linux CI and make the missing backend a runtime error with a clear message
  # instead of a link error nobody can read.
  #
  # The audio device is the exception: it is one file for every platform, so
  # Linux gets a working one for free. That is worth more than it sounds --
  # it means the device wrapper is exercised by a real backend in CI, on a
  # machine nobody has to plug speakers into.
  target_sources(sonora_platform PRIVATE
    linux/window_stub.cpp
    linux/event_loop_stub.cpp
    linux/child_window_stub.cpp
    linux/paths_stub.cpp
    linux/arguments_stub.cpp
    linux/media_integration_stub.cpp
    shared/displays_none.cpp
    shared/shell_integration_none.cpp
    shared/single_instance_none.cpp
    shared/url_scheme_none.cpp)
endif()
