include(ExternalIncludes)

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
  cef/desktop.cpp
  cef/event_channel.cpp
  cef/handlers.cpp
  cef/library_host.cpp
  cef/media_session.cpp
  cef/player_host.cpp
  cef/render_process.cpp
  cef/runtime.cpp
  cef/scheme_handler.cpp
  cef/timer.cpp)

target_include_directories(sonora_cef PUBLIC
  "${CMAKE_CURRENT_LIST_DIR}")  # so #include "cef/app.h" resolves

# "${CEF_ROOT}" is what makes #include "include/cef_app.h" resolve, and it comes
# in as someone else's code rather than as an ordinary include path -- see
# cmake/ExternalIncludes.cmake for what that means on MSVC and why it took a
# week of C4100 noise to learn. Week 5 uses the same helper for miniaudio.
sonora_target_external_include(sonora_cef PUBLIC "${CEF_ROOT}")

# libcef_lib is the imported import-library, created by ADD_LOGICAL_TARGET in
# CefTargets.cmake; find_package(CEF) defines variables and macros but no
# targets. CEF_STANDARD_LIBS is the platform's own set (comctl32, shlwapi,
# ws2_32 and friends) that libcef.dll expects the host to link.
target_link_libraries(sonora_cef PUBLIC
  sonora::assets
  sonora::audio
  sonora::bridge
  sonora::library
  sonora::state
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
add_executable(sonora main.cpp play_command.cpp)
set_target_properties(sonora PROPERTIES OUTPUT_NAME "Sonora")
target_link_libraries(sonora PRIVATE sonora_cef)
add_dependencies(sonora sonora_helper)

target_compile_definitions(sonora PRIVATE
  SONORA_HELPER_NAME="sonora_helper${CMAKE_EXECUTABLE_SUFFIX}"
  $<$<CONFIG:Debug>:SONORA_ENABLE_DEVTOOLS>)

if(WIN32)
  # The .rc is generated so the version in it cannot drift from the project's:
  # there is one version number, in the top-level project() call, and the
  # executable's properties, the media panel's name and --version all read it.
  set(SONORA_RC_DIR "${CMAKE_CURRENT_LIST_DIR}")
  configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/sonora.rc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/sonora.rc"
    @ONLY)
  target_sources(sonora PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/sonora.rc")
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

# ---------------------------------------------------------------------------
# What ships
# ---------------------------------------------------------------------------
# vcpkg installs a Release and a Debug tree; the dependency scan below is told
# about both because the same rules serve both presets, and a directory that
# does not exist is simply never matched.
if(DEFINED _VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
  set(SONORA_VCPKG_BIN_DIRS
    "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin"
    "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/bin")
else()
  set(SONORA_VCPKG_BIN_DIRS "")
endif()

# The installer harvests the install tree, not the build directory, and the
# difference is the reason these rules exist: bin/Release also holds
# sonora_tests.exe, every object file's leftovers and whatever else a build
# leaves lying around. An installer that ships its own test runner is an
# installer nobody read.
#
# CEF_BINARY_FILES and CEF_RESOURCE_FILES are CEF's own lists, the same ones
# sonora_copy_cef_runtime copies next to the executable -- so the set that
# ships and the set that lets a build run locally cannot drift apart. One of
# the resource entries is a directory (locales), which is why this loop asks.
if(WIN32)
  # RUNTIME_DEPENDENCY_SET, and this is the part that was wrong the first time.
  #
  # The rules below listed CEF's files and the two executables, which is what a
  # person remembers. They did not list sqlite3.dll, tag.dll and z.dll, which
  # vcpkg copies next to the executable after the link and which CMake
  # therefore knows nothing about. The MSI built, installed, and the
  # application it installed died on launch with "sqlite3.dll was not found" --
  # exactly the failure the comment in tools/gen_installer_files.py describes,
  # one step further up the pipeline, in a list I had written by hand while
  # writing that comment.
  #
  # So the binaries are asked instead. CMake reads the import tables at install
  # time and brings what they name, which means a dependency added next month
  # ships without anybody remembering it.
  # RUNTIME_DEPENDENCY_SET before RUNTIME DESTINATION, and not by taste: the
  # signature takes the set name among the arguments that apply to the whole
  # call, and once parsing has entered a per-artifact block (RUNTIME, LIBRARY,
  # ARCHIVE) anything that belongs outside it is simply "an unknown argument".
  install(TARGETS sonora sonora_helper
    RUNTIME_DEPENDENCY_SET sonora_runtime
    RUNTIME DESTINATION ".")

  install(RUNTIME_DEPENDENCY_SET sonora_runtime
    PRE_EXCLUDE_REGEXES
      # API sets are not files; nothing can be copied for them.
      "api-ms-win-.*"
      "ext-ms-.*"
      # CEF's own, installed from CEF's own list below. Excluding it here also
      # stops the scan from recursing into a 200 MB library to rediscover the
      # files that list already names.
      "^libcef\\.dll$"
      "^chrome_elf\\.dll$"
    POST_EXCLUDE_REGEXES
      # Anything that resolved out of Windows itself. The Visual C++ runtime is
      # in here, which is a real dependency being left to the machine: it is
      # present on any Windows that has ever installed a C++ application, and
      # shipping it app-locally is a decision for the day a clean-machine
      # install fails, not a guess made now.
      "[Ss]ystem32"
      "[Ss]ys[Ww]ow64"
    DIRECTORIES
      # Unquoted: it is a list, and quoting it would hand CMake one directory
      # with a semicolon in its name.
      ${SONORA_VCPKG_BIN_DIRS}
      "${CEF_BINARY_DIR}"
    DESTINATION ".")

  foreach(cef_file IN LISTS CEF_BINARY_FILES)
    install(FILES "${CEF_BINARY_DIR}/${cef_file}" DESTINATION ".")
  endforeach()

  foreach(cef_file IN LISTS CEF_RESOURCE_FILES)
    if(IS_DIRECTORY "${CEF_RESOURCE_DIR}/${cef_file}")
      install(DIRECTORY "${CEF_RESOURCE_DIR}/${cef_file}" DESTINATION ".")
    else()
      install(FILES "${CEF_RESOURCE_DIR}/${cef_file}" DESTINATION ".")
    endif()
  endforeach()

  # Chromium's licence obligations are not optional, and neither is ours. Two
  # calls rather than one list, because the second needs a name that does not
  # overwrite the first.
  install(FILES "${CMAKE_SOURCE_DIR}/LICENSE" DESTINATION ".")
  install(FILES "${CEF_ROOT}/LICENSE.txt" DESTINATION "."
          RENAME "LICENSE-cef.txt" OPTIONAL)
endif()
