# The web UI, and whether the build is allowed to build it.
#
# Until now it was not: `cmake --build` embedded whatever happened to be in
# ui/dist, and if that was three weeks old you found out at runtime, by reading
# an interface that did not have the button you had just added. That was a known
# debt from week 7 and it becomes a real problem in week 10, because an
# installer must not be able to ship a stale interface -- a release is the one
# build nobody gets to "just rebuild and try again".
#
# Two halves, and the split is deliberate:
#
#   SONORA_BUILD_UI=OFF (the default)  -- the build does not run npm, and the
#       configure step warns when ui/dist is older than ui/src. A developer
#       running `npm run dev` in another terminal already has a fresher dist
#       than any build step could produce, and having CMake fight that watcher
#       for the same directory is how a build starts rebuilding things nobody
#       changed.
#
#   SONORA_BUILD_UI=ON  -- npm ci and npm run build run as a build step that
#       everything embedding the UI depends on. This is what CI and the release
#       build use, and it is the only mode in which "the binary contains the
#       interface in this commit" is a property rather than a hope.

include_guard(GLOBAL)

option(SONORA_BUILD_UI "Run npm to build ui/dist as part of the build" OFF)

set(SONORA_UI_DIR "${CMAKE_SOURCE_DIR}/ui")
set(SONORA_UI_DIST "${SONORA_UI_DIR}/dist")

# Newest modification time under `dir`, or 0 when there is nothing there.
function(sonora_newest_timestamp dir out_var)
  file(GLOB_RECURSE files "${dir}/*")
  set(newest "0")
  foreach(file IN LISTS files)
    if(NOT IS_DIRECTORY "${file}")
      file(TIMESTAMP "${file}" stamp "%Y%m%d%H%M%S")
      if(stamp STRGREATER newest)
        set(newest "${stamp}")
      endif()
    endif()
  endforeach()
  set(${out_var} "${newest}" PARENT_SCOPE)
endfunction()

function(sonora_check_ui_freshness)
  if(NOT EXISTS "${SONORA_UI_DIST}")
    message(WARNING
      "ui/dist does not exist: the embedded interface will be empty.\n"
      "  Run `npm ci && npm run build` in ui/, or configure with -DSONORA_BUILD_UI=ON.")
    return()
  endif()

  sonora_newest_timestamp("${SONORA_UI_DIR}/src" newest_source)
  sonora_newest_timestamp("${SONORA_UI_DIST}" newest_built)

  if(newest_source STRGREATER newest_built)
    message(WARNING
      "ui/dist is older than ui/src: this build will embed a stale interface.\n"
      "  Run `npm run build` in ui/, or configure with -DSONORA_BUILD_UI=ON.")
  endif()
endfunction()

# Defines the target `sonora_ui` when SONORA_BUILD_UI is ON, and nothing
# otherwise. Callers depend on it through sonora_ui_dependency() so they do not
# have to know which mode they are in.
function(sonora_define_ui_target)
  if(NOT SONORA_BUILD_UI)
    sonora_check_ui_freshness()
    return()
  endif()

  find_program(SONORA_NPM_EXECUTABLE npm)
  if(NOT SONORA_NPM_EXECUTABLE)
    message(FATAL_ERROR
      "SONORA_BUILD_UI is ON but npm was not found. Install Node 22 or newer, "
      "or configure with -DSONORA_BUILD_UI=OFF and build ui/dist by hand.")
  endif()

  # A stamp file rather than ui/dist itself: a directory's timestamp is not a
  # reliable statement about its contents, and a custom command whose output is
  # a directory re-runs on every build.
  set(stamp "${CMAKE_BINARY_DIR}/ui-build.stamp")
  file(GLOB_RECURSE ui_sources CONFIGURE_DEPENDS
    "${SONORA_UI_DIR}/src/*"
    "${SONORA_UI_DIR}/index.html"
    "${SONORA_UI_DIR}/package.json"
    "${SONORA_UI_DIR}/package-lock.json"
    "${SONORA_UI_DIR}/tsconfig.json"
    "${SONORA_UI_DIR}/vite.config.ts")

  # `cmd /c call` on Windows, and it is not decoration.
  #
  # npm is npm.cmd there, CMake writes a custom command's steps into a batch
  # file, and a batch file that invokes another batch file *without* `call`
  # does not come back: the second one replaces the first, and every step after
  # it is silently skipped. That is exactly what happened the first time this
  # ran -- `npm ci` printed its output, `npm run build` never ran, the stamp was
  # never written, and the only trace was an MSBuild warning about an output
  # that had not been created. The build then embedded the ui/dist that
  # happened to be lying around, and said "Embedding the web UI" while doing it.
  if(WIN32)
    set(npm ${CMAKE_COMMAND} -E env cmd /c call "${SONORA_NPM_EXECUTABLE}")
  else()
    set(npm "${SONORA_NPM_EXECUTABLE}")
  endif()

  # `npm ci` rather than `npm install`: it installs exactly what the lock file
  # says and fails if the two disagree, which is the property a release build
  # needs and the one `npm install` does not offer.
  #
  # The last step copies the built index.html to the stamp rather than touching
  # a file into existence. Same timestamp, one property more: if the UI build
  # did not produce an index.html, the copy fails and the build stops. A stamp
  # that can appear whether or not the work happened certifies nothing -- which
  # is how the bug above stayed invisible.
  add_custom_command(
    OUTPUT "${stamp}"
    COMMAND ${npm} ci
    COMMAND ${npm} run build
    COMMAND "${CMAKE_COMMAND}" -E copy "${SONORA_UI_DIST}/index.html" "${stamp}"
    WORKING_DIRECTORY "${SONORA_UI_DIR}"
    DEPENDS ${ui_sources}
    COMMENT "Building the web UI (npm ci && npm run build)"
    VERBATIM)

  add_custom_target(sonora_ui DEPENDS "${stamp}")
endfunction()

# The dependency to attach to anything that reads ui/dist. Empty when the build
# is not building the UI, so the call site has no branch.
function(sonora_ui_dependency out_var)
  if(TARGET sonora_ui)
    set(${out_var} sonora_ui PARENT_SCOPE)
  else()
    set(${out_var} "" PARENT_SCOPE)
  endif()
endfunction()
