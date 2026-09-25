# Bringing a third-party header in as someone else's code.
#
# This exists because of week 3. CEF's headers were added with
# target_include_directories, which put them under this project's /W4, and
# every CEF handler base class -- full of default implementations that name
# their parameters and use none of them -- produced a C4100. Hundreds of
# warnings in files nobody here will ever edit, and the real one hides in them.
#
# The fix is to say the headers are external, not to switch the warning off:
# /wd4100 on the target would also cover our own code, where an unused
# parameter is a genuine signal.
#
# On MSVC that means /external:I rather than /I, and two things have to be true
# together or it silently does nothing:
#
#   * the directory must NOT also be an ordinary include directory -- /I is
#     searched first, and a header found there is not external;
#   * /external:W0 is the other half of the same setting. /external:I alone
#     changes no warning level at all.
#
# The flags are written out rather than relying on the SYSTEM keyword of
# target_include_directories, whose mapping onto /external: depends on the
# CMake version and the generator. This works with any of them, and it fails
# loudly if it does not work: the include stops resolving and the build stops,
# rather than quietly going back to being noisy.

include_guard(GLOBAL)

function(sonora_target_external_include target visibility)
  foreach(directory ${ARGN})
    if(MSVC)
      target_compile_options(${target} ${visibility}
        /external:W0
        "SHELL:/external:I \"${directory}\"")
    else()
      target_include_directories(${target} SYSTEM ${visibility} "${directory}")
    endif()
  endforeach()
endfunction()
