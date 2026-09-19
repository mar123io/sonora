cmake_minimum_required(VERSION 3.28)

project(Sonora
  VERSION 0.1.0
  DESCRIPTION "Native C++ desktop shell hosting a web UI"
  LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

# Objective-C++ is only needed for the macOS platform layer.
if(APPLE)
  enable_language(OBJCXX)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(SONORA_BUILD_SHELL "Build the sonora executable (needs a platform window layer)" ON)
option(SONORA_BUILD_TESTS "Build unit tests" ON)
option(SONORA_ENABLE_CEF  "Download and link the Chromium Embedded Framework" OFF)

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
# Version stamp: <project version>+<git describe>, "unknown" outside a checkout
# ---------------------------------------------------------------------------
find_package(Git QUIET)
set(SONORA_GIT_DESCRIBE "unknown")
if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/.git")
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" describe --tags --always --dirty
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
    OUTPUT_VARIABLE SONORA_GIT_DESCRIBE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(NOT SONORA_GIT_DESCRIBE)
    set(SONORA_GIT_DESCRIBE "unknown")
  endif()
endif()
message(STATUS "Sonora ${PROJECT_VERSION} (${SONORA_GIT_DESCRIBE})")

enable_testing()

add_subdirectory(src)

if(SONORA_BUILD_TESTS)
  add_subdirectory(tests)
endif()
