# CEF's CMake scripts set flags for the directory they are included from.
# Everything configured before this line (core, assets, platform) is untouched
# by them -- see cmake/CefTargets.cmake.
include(CefTargets)

# ---------------------------------------------------------------------------
# sonora_cef: the CEF integration, shared by the application and the helper
# ---------------------------------------------------------------------------
add_library(sonora_cef STATIC
  cef/app.cpp
  cef/client.cpp
  cef/runtime.cpp
  cef/scheme_handler.cpp)

target_include_directories(sonora_cef PUBLIC
  "${CMAKE_CURRENT_LIST_DIR}"   # so #include "cef/app.h" resolves
  "${CEF_ROOT}")                # so #include "include/cef_app.h" resolves

# libcef_lib is the imported import-library, created by ADD_LOGICAL_TARGET in
# CefTargets.cmake; find_package(CEF) defines variables and macros but no
# targets. CEF_STANDARD_LIBS is the platform's own set (comdlg32, shlwapi,
# ws2_32 and friends) that libcef.dll expects the host to link.
target_link_libraries(sonora_cef PUBLIC
  sonora::assets
  sonora::platform
  sonora::core
  libcef_lib
  libcef_dll_wrapper
  ${CEF_STANDARD_LIBS})

if(COMMAND SET_LIBRARY_TARGET_PROPERTIES)
  SET_LIBRARY_TARGET_PROPERTIES(sonora_cef)
endif()

# ---------------------------------------------------------------------------
# The helper: one small executable that hosts CEF's child processes
# ---------------------------------------------------------------------------
add_executable(sonora_helper helper_main.cpp)
set_target_properties(sonora_helper PROPERTIES OUTPUT_NAME "sonora_helper")
target_link_libraries(sonora_helper PRIVATE sonora_cef)

# ---------------------------------------------------------------------------
# The application
# ---------------------------------------------------------------------------
add_executable(sonora main.cpp)
set_target_properties(sonora PROPERTIES OUTPUT_NAME "Sonora")
target_link_libraries(sonora PRIVATE sonora_cef)
add_dependencies(sonora sonora_helper)

target_compile_definitions(sonora PRIVATE
  SONORA_HELPER_NAME="sonora_helper${CMAKE_EXECUTABLE_SUFFIX}"
  $<$<CONFIG:Debug>:SONORA_ENABLE_DEVTOOLS>)

if(WIN32)
  target_sources(sonora PRIVATE sonora.rc)
endif()

# CEF's macro sets the LINK_FLAGS property outright rather than appending, so
# it has to run before anything else touches linker settings. It already
# includes /MANIFEST:NO, which is what our .rc-embedded manifest needs; setting
# it by hand here would be overwritten by this call.
foreach(sonora_target sonora sonora_helper)
  if(COMMAND SET_EXECUTABLE_TARGET_PROPERTIES)
    SET_EXECUTABLE_TARGET_PROPERTIES(${sonora_target})
  endif()
endforeach()

if(MSVC)
  # target_link_options writes LINK_OPTIONS, a different property from the
  # LINK_FLAGS that CEF's macro owns, so the two do not fight.
  #
  # /ENTRY is not optional next to /SUBSYSTEM:WINDOWS. The linker's default
  # startup for that subsystem is WinMainCRTStartup, which looks for the ANSI
  # WinMain; the platform layer defines the wide wWinMain, so the startup has
  # to be the wide one too or the link fails on a symbol nobody wrote.
  #
  # /IGNORE:4199 silences one LNK4199 per delay-loaded DLL. CEF's linker flags
  # delay-load a long list of system libraries that the browser process needs,
  # and a target that imports from none of them warns about each one. The
  # warnings say nothing about this code.
  set(sonora_windows_subsystem
    $<$<NOT:$<CONFIG:Debug>>:/SUBSYSTEM:WINDOWS>
    $<$<NOT:$<CONFIG:Debug>>:/ENTRY:wWinMainCRTStartup>)

  # Console subsystem in Debug so printf and CEF's warnings are visible while
  # developing, windowed in Release. main() and wWinMain() are both defined in
  # the platform layer; the entry point setting picks which one is used.
  target_link_options(sonora PRIVATE
    $<$<CONFIG:Debug>:/SUBSYSTEM:CONSOLE>
    ${sonora_windows_subsystem}
    /IGNORE:4199)

  # The helper is never the thing a user launches: no console, in any config.
  target_link_options(sonora_helper PRIVATE
    /SUBSYSTEM:WINDOWS
    /ENTRY:wWinMainCRTStartup
    /IGNORE:4199)
endif()

# libcef.dll and its resources must sit next to the executable, or the
# application starts and exits immediately with no message.
sonora_copy_cef_runtime(sonora)
