# CEF's CMake scripts set flags for the directory they are included from.
# Everything configured before this line (core, assets, bridge, platform) is
# untouched by them -- see cmake/CefTargets.cmake.
include(CefTargets)

# ---------------------------------------------------------------------------
# sonora_cef: the CEF integration, shared by the application and the helper
# ---------------------------------------------------------------------------
add_library(sonora_cef STATIC
  cef/app.cpp
  cef/bridge_router.cpp
  cef/client.cpp
  cef/handlers.cpp
  cef/render_process.cpp
  cef/runtime.cpp
  cef/scheme_handler.cpp)

target_include_directories(sonora_cef PUBLIC
  "${CMAKE_CURRENT_LIST_DIR}")  # so #include "cef/app.h" resolves

# ---------------------------------------------------------------------------
# CEF's headers come in as external headers, not as ordinary include paths
# ---------------------------------------------------------------------------
# "${CEF_ROOT}" is what makes #include "include/cef_app.h" resolve. Adding it
# with target_include_directories put it under this project's /W4, and CEF's
# handler base classes are full of default implementations that name their
# parameters and use none of them:
#
#     virtual void OnLoadStart(CefRefPtr<CefBrowser> browser, ...) {}
#
# which is hundreds of C4100 warnings per target, in files nobody here will
# ever edit. Warnings that cannot be acted on are worse than no warnings: they
# are what the real one hides in.
#
# The fix is to say these headers are someone else's, not to switch C4100 off.
# /wd4100 on this target would also cover cef/*.cpp, where an unused parameter
# is a genuine signal that a handler is ignoring something it was given.
#
# On MSVC that means /external:I rather than /I, and the path must NOT also be
# an ordinary include directory: /I is searched first and a header found there
# is not external. /external:W0 is the other half of the same setting -- it is
# what lowers the warning level for those files, and /external:I alone does
# nothing at all.
#
# The flags are written out rather than relying on the SYSTEM keyword of
# target_include_directories, whose mapping onto /external: depends on the
# CMake version and the generator. This works with any of them.
if(MSVC)
  target_compile_options(sonora_cef PUBLIC
    /external:W0
    "SHELL:/external:I \"${CEF_ROOT}\"")
else()
  target_include_directories(sonora_cef SYSTEM PUBLIC "${CEF_ROOT}")
endif()

# libcef_lib is the imported import-library, created by ADD_LOGICAL_TARGET in
# CefTargets.cmake; find_package(CEF) defines variables and macros but no
# targets. CEF_STANDARD_LIBS is the platform's own set (comctl32, shlwapi,
# ws2_32 and friends) that libcef.dll expects the host to link.
target_link_libraries(sonora_cef PUBLIC
  sonora::assets
  sonora::bridge
  sonora::platform
  sonora::core
  libcef_lib
  libcef_dll_wrapper
  ${CEF_STANDARD_LIBS})

# ---------------------------------------------------------------------------
# Why CEF's SET_LIBRARY_TARGET_PROPERTIES / SET_EXECUTABLE_TARGET_PROPERTIES
# are NOT used on these targets
# ---------------------------------------------------------------------------
# Those macros call SET_COMMON_TARGET_PROPERTIES, which applies
# CEF_COMPILER_DEFINES to whatever target they are given. That list contains
#
#     _HAS_EXCEPTIONS=0    # Disable exceptions
#
# along with /GR- and no /EH option at all. Those describe how CEF's own source
# is written. They are not a description of this project's code.
#
# Applied to sonora_cef they compiled cef/handlers.cpp -- which signals failure
# by throwing bridge::BridgeError -- with exception support switched off, while
# the catch lives in sonora_bridge, compiled normally. Throwing across that
# boundary is undefined behaviour, and it behaved like it: the error arrived in
# the page with code 0xCDCDCDCD (uninitialised heap fill) and a garbage message.
#
# The lesson generalises past CEF: a dependency's build macros encode the
# dependency's language policy, and adopting them wholesale silently adopts
# that policy for your code too.
#
# What those macros also brought, and what is re-added explicitly below, is the
# handful of linker settings this application actually needs. The long
# /DELAYLOAD list goes away with them, and so do the 60 LNK4199 warnings it
# produced -- which is why there is no /IGNORE:4199 here any more.

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

if(MSVC)
  foreach(sonora_target sonora sonora_helper)
    # The application manifest is embedded through the .rc file, so MSVC must
    # not generate one of its own or the two collide at link time.
    #
    # Chromium expects a larger main-thread stack than the 1 MB default; this
    # is the one value worth carrying over from CEF's linker flags.
    target_link_options(${sonora_target} PRIVATE /MANIFEST:NO /STACK:0x800000)
  endforeach()

  # /ENTRY is not optional next to /SUBSYSTEM:WINDOWS. The linker's default
  # startup for that subsystem is WinMainCRTStartup, which looks for the ANSI
  # WinMain; the platform layer defines the wide wWinMain, so the startup has
  # to be the wide one too or the link fails on a symbol nobody wrote.
  #
  # Console subsystem in Debug so printf and CEF's warnings are visible while
  # developing, windowed in Release. main() and wWinMain() are both defined in
  # the platform layer; the entry point setting picks which one is used.
  target_link_options(sonora PRIVATE
    $<$<CONFIG:Debug>:/SUBSYSTEM:CONSOLE>
    $<$<NOT:$<CONFIG:Debug>>:/SUBSYSTEM:WINDOWS>
    $<$<NOT:$<CONFIG:Debug>>:/ENTRY:wWinMainCRTStartup>)

  # The helper is never the thing a user launches: no console, in any config.
  target_link_options(sonora_helper PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup)
endif()

# libcef.dll and its resources must sit next to the executable, or the
# application starts and exits immediately with no message.
sonora_copy_cef_runtime(sonora)
