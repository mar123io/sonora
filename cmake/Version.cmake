# The version number, derived from the git tag rather than typed twice.
#
# Included *before* project(), because project(VERSION ...) is where the number
# has to arrive: everything downstream -- the executable's VERSIONINFO
# resource, --version, the installer's ProductVersion, the name of the file a
# release attaches -- reads it from there, and a second place to edit is a
# second place to forget.
#
# What counts as a release tag: vMAJOR.MINOR.PATCH, and nothing else. This
# repository also carries narrative tags for the milestones -- v0.3-player,
# v0.4-native -- and those are not releases; describing against them would make
# the version number say "0.4-native-2-g1fd9823", which is a sentence, not a
# version. The --match pattern is what keeps the two kinds of tag apart, and it
# is why the milestones can go on being named after what they are.
#
# Outputs:
#   SONORA_VERSION          x.y.z, for project(VERSION ...)
#   SONORA_VERSION_TWEAK    commits since that tag (0 on the tag itself)
#   SONORA_GIT_DESCRIBE     the full description, or "unknown" outside a checkout
#   SONORA_VERSION_IS_TAGGED  TRUE when this commit is exactly a release tag
#
# A build from a source tarball, with no .git anywhere, is a real case and not
# an error: it takes the fallback below and says so.

include_guard(GLOBAL)

# What an untagged build calls itself. Bumped when a release is cut, and the
# only version number written by hand in the repository.
set(SONORA_FALLBACK_VERSION "0.5.0")

set(SONORA_VERSION "${SONORA_FALLBACK_VERSION}")
set(SONORA_VERSION_TWEAK 0)
set(SONORA_GIT_DESCRIBE "unknown")
set(SONORA_VERSION_IS_TAGGED FALSE)

# find_program rather than find_package(Git): this runs before project(), and
# before project() there is no compiler, no CMAKE_SYSTEM and not much of the
# machinery a find module is entitled to assume.
find_program(SONORA_GIT_EXECUTABLE git)

if(SONORA_GIT_EXECUTABLE AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/../.git")
  # --long so the output shape is the same on a tag and off it: v1.2.3-0-gabc
  # rather than v1.2.3. One shape is one regular expression.
  execute_process(
    COMMAND "${SONORA_GIT_EXECUTABLE}" describe
            --tags --long --dirty --match "v[0-9]*.[0-9]*.[0-9]*"
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/.."
    OUTPUT_VARIABLE sonora_release_describe
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE sonora_describe_result
    ERROR_QUIET)

  if(sonora_describe_result EQUAL 0 AND
     sonora_release_describe MATCHES "^v([0-9]+)\\.([0-9]+)\\.([0-9]+)-([0-9]+)-g[0-9a-f]+(-dirty)?$")
    set(SONORA_VERSION "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")
    set(SONORA_VERSION_TWEAK "${CMAKE_MATCH_4}")
    # Exactly on the tag, and nothing edited since. A dirty tree is not a
    # release even when it stands on one, and the installer's file name says so.
    if(SONORA_VERSION_TWEAK EQUAL 0 AND NOT CMAKE_MATCH_5)
      set(SONORA_VERSION_IS_TAGGED TRUE)
    endif()
  endif()

  # The human-readable description keeps *all* the tags, milestones included:
  # it is what --version prints and what a bug report quotes, and there the
  # narrative name is the useful half.
  execute_process(
    COMMAND "${SONORA_GIT_EXECUTABLE}" describe --tags --always --dirty
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/.."
    OUTPUT_VARIABLE sonora_full_describe
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(sonora_full_describe)
    set(SONORA_GIT_DESCRIBE "${sonora_full_describe}")
  endif()
endif()
