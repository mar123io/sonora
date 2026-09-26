cmake_minimum_required(VERSION 3.28)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

if(POLICY CMP0207)
  # file(GET_RUNTIME_DEPENDENCIES) compares a dependency's path against the
  # exclude patterns. Under the old behaviour it compares the path as Windows
  # handed it over -- "C:\WINDOWS\system32/kernel32.dll", with both kinds of
  # separator in one string -- and under the new one it normalises first.
  #
  # The exclusions in src/shell/CMakeLists.txt happen to work either way,
  # because none of them contains a separator. Taking the new behaviour is
  # still the right answer: the warning is CMake saying "this rule depends on
  # a detail you did not choose", and a rule about paths that behaves
  # differently depending on who wrote the slashes is a rule waiting to be
  # wrong on the next dependency.
  cmake_policy(SET CMP0207 NEW)
endif()

# Before project(), because project(VERSION ...) is where the number has to
# arrive. See cmake/Version.cmake for what counts as a release tag and why the
# milestone tags are deliberately not one.
include(Version)

project(Sonora
  VERSION ${SONORA_VERSION}
  DESCRIPTION "Native C++ desktop shell hosting a web UI"
  LANGUAGES CXX)

# Objective-C++ is only needed for the macOS platform layer.
if(APPLE)
  enable_language(OBJCXX)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
# Building the shell implies CEF: from week 2 the executable is a CEF host and
# there is no meaningful build of it without one. The macOS and Linux CI jobs
# turn this off and check only the portable targets, which is what keeps the
# platform abstraction honest without downloading a gigabyte per run.
option(SONORA_BUILD_SHELL "Build the Sonora executable (requires CEF)" ON)
option(SONORA_BUILD_TESTS "Build unit tests" ON)

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /EHsc /Zc:__cplusplus)
  add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# What this build calls itself
# ---------------------------------------------------------------------------
# Every number here came out of cmake/Version.cmake, which read the git tag. The
# line is printed rather than left implicit: a release that turns out to have
# been built from an untagged or dirty tree is worth noticing before it is
# published rather than after.
if(SONORA_VERSION_IS_TAGGED)
  message(STATUS "Sonora ${PROJECT_VERSION} (${SONORA_GIT_DESCRIBE}) -- release build")
else()
  message(STATUS "Sonora ${PROJECT_VERSION}+${SONORA_VERSION_TWEAK} (${SONORA_GIT_DESCRIBE})")
endif()

# The web UI: built by this build only when asked, checked for staleness when
# not. See cmake/WebUI.cmake.
include(WebUI)
sonora_define_ui_target()

enable_testing()

add_subdirectory(src)

if(SONORA_BUILD_TESTS)
  add_subdirectory(tests)
endif()
