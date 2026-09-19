add_executable(sonora main.cpp)
set_target_properties(sonora PROPERTIES OUTPUT_NAME "Sonora")

target_link_libraries(sonora PRIVATE sonora::platform sonora::core)

if(WIN32)
  target_sources(sonora PRIVATE sonora.rc)
endif()

if(MSVC)
  # The application manifest is embedded through the .rc file, so MSVC must not
  # generate one of its own or the two collide at link time.
  set_property(TARGET sonora APPEND_STRING PROPERTY LINK_FLAGS " /MANIFEST:NO")

  # Console subsystem in Debug so printf/stderr are visible while developing,
  # windowed subsystem in Release so shipping builds do not flash a console.
  # main() and wWinMain() are both defined; the linker picks the one that
  # matches the subsystem.
  target_link_options(sonora PRIVATE
    $<$<CONFIG:Debug>:/SUBSYSTEM:CONSOLE>
    $<$<NOT:$<CONFIG:Debug>>:/SUBSYSTEM:WINDOWS>)
endif()

# Week 2: flip SONORA_ENABLE_CEF on and this block provisions the binary
# distribution and links libcef + libcef_dll_wrapper into the shell.
if(SONORA_ENABLE_CEF)
  include(DownloadCEF)
  sonora_provision_cef()
  message(STATUS "CEF provisioned at ${CEF_ROOT}")
endif()
