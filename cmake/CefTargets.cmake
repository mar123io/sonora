# Brings the Chromium Embedded Framework into the build.
#
# Included from src/shell/CMakeLists.txt and not from the top level on purpose:
# CEF's own CMake scripts set compiler and linker flags for the directory that
# includes them, and core, assets and platform are configured before this point
# so none of it reaches them.
#
# Everything here was checked against the scripts shipped inside CEF 152's
# binary distribution (cmake/cef_variables.cmake, cmake/cef_macros.cmake),
# not against the documentation.

include_guard(GLOBAL)

include(DownloadCEF)
sonora_provision_cef()

if(NOT CEF_ROOT OR NOT EXISTS "${CEF_ROOT}")
  message(FATAL_ERROR "CEF was not provisioned; see cmake/DownloadCEF.cmake.")
endif()

# ---------------------------------------------------------------------------
# Settings that must be in the cache BEFORE find_package(CEF)
# ---------------------------------------------------------------------------
# cef_variables.cmake declares both of these itself
#   set(CEF_RUNTIME_LIBRARY_FLAG "/MT" CACHE STRING ...)
#   option(USE_SANDBOX "..." ON)
# so the only way to change them is to put a value in the cache first.
#
# /MD rather than /MT: vcpkg's x64-windows triplet builds against the dynamic
# CRT, and a single binary cannot contain both. The symptoms of getting this
# wrong are duplicate-symbol errors if you are lucky and heap corruption if you
# are not.
set(CEF_RUNTIME_LIBRARY_FLAG "/MD" CACHE STRING "CRT flag CEF builds against" FORCE)

# The sandbox needs cef_sandbox.lib, published only for the static CRT, so it
# is incompatible with the line above. This is a real security cost, recorded
# in ADR 0003 rather than hidden in a comment here.
set(USE_SANDBOX OFF CACHE BOOL "Link cef_sandbox (needs the static CRT)" FORCE)

# ATL is only used by CEF's own sample applications.
set(USE_ATL OFF CACHE BOOL "Enable ATL support in CEF's scripts" FORCE)

list(APPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
find_package(CEF REQUIRED)

# ---------------------------------------------------------------------------
# The minimal distribution ships Release binaries only
# ---------------------------------------------------------------------------
# cef_variables.cmake points CEF_LIB_DEBUG at <root>/Debug/libcef.lib
# unconditionally, and that directory does not exist in a minimal
# distribution. Rather than pulling down the much larger standard
# distribution, a Debug build of Sonora links CEF's Release binaries.
#
# This is safe because libcef.dll exposes a C API and owns its own
# allocations, so a /MDd application and a /MD library never share a heap
# across that boundary. The cost is that you cannot step into Chromium, which
# nobody wants to do anyway. The check is conditional, so pinning a standard
# distribution later picks the real Debug binaries back up with no edit here.
if(NOT EXISTS "${CEF_LIB_DEBUG}")
  message(STATUS "CEF minimal distribution: Debug builds will link the Release binaries.")
  set(CEF_LIB_DEBUG "${CEF_LIB_RELEASE}")
  set(CEF_BINARY_DIR_DEBUG "${CEF_BINARY_DIR_RELEASE}")
  set(CEF_BINARY_DIR "${CEF_BINARY_DIR_RELEASE}")
endif()

# find_package(CEF) defines variables and macros but no targets: the imported
# library is created by the application, which is what CEF's own examples do.
if(NOT TARGET libcef_lib)
  ADD_LOGICAL_TARGET("libcef_lib" "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")
endif()

# libcef_dll_wrapper is the C++ layer over CEF's C API. It ships as source
# precisely because it must be compiled with the same compiler and CRT as the
# application that links it.
add_subdirectory("${CEF_LIBCEF_DLL_WRAPPER_PATH}" libcef_dll_wrapper)

# Used by CEF's macros that post-process an executable in place.
set(CEF_TARGET_OUT_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>")

PRINT_CEF_CONFIG()

# Copies the CEF runtime (libcef.dll, the v8 snapshot, the .pak resources and
# the locales) next to a target's executable. Without this the application
# starts and exits immediately with no message.
function(sonora_copy_cef_runtime target)
  COPY_FILES("${target}" "${CEF_BINARY_FILES}" "${CEF_BINARY_DIR}"
             "$<TARGET_FILE_DIR:${target}>")
  COPY_FILES("${target}" "${CEF_RESOURCE_FILES}" "${CEF_RESOURCE_DIR}"
             "$<TARGET_FILE_DIR:${target}>")
endfunction()
