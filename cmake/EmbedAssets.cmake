# Turns the built web UI into a C++ translation unit.
#
# The generator runs at build time, not configure time, so rebuilding after
# `npm run build` picks up the new UI without re-running CMake. It writes the
# file only when the content actually changed, so an unchanged UI does not drag
# the whole shell through a rebuild.

include_guard(GLOBAL)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

function(sonora_embed_ui_assets out_var)
  set(generated "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_assets.cpp")
  set(ui_dist "${CMAKE_SOURCE_DIR}/ui/dist")
  set(script "${CMAKE_SOURCE_DIR}/tools/embed_assets.py")

  # The glob is only used to decide when to re-run; CONFIGURE_DEPENDS makes
  # CMake re-check it at build time instead of only at configure time.
  file(GLOB_RECURSE ui_files CONFIGURE_DEPENDS "${ui_dist}/*")

  add_custom_command(
    OUTPUT "${generated}"
    COMMAND "${Python3_EXECUTABLE}" "${script}" --input "${ui_dist}" --output "${generated}"
    DEPENDS "${script}" ${ui_files}
    COMMENT "Embedding the web UI"
    VERBATIM)

  set(${out_var} "${generated}" PARENT_SCOPE)
endfunction()
