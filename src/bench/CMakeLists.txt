# The benchmark harness's arithmetic: medians, spreads, and the rule that decides whether
# one measurement is worse than another.
#
# A library and not part of the runner, for the same reason NextStep is a library and not
# part of sonora-updater: the piece most likely to be wrong here is not the timing, it is
# the comparison -- and a comparison living inside a tool that only runs in CI is a
# comparison nobody ever tests. This one has a test file pointed at it and it measures
# nothing at all.
#
# Portable, like everything else that is only arithmetic. The macOS and Linux jobs build it
# and run its tests.
find_package(nlohmann_json CONFIG REQUIRED)

add_library(sonora_bench STATIC
  src/measure.cpp)
add_library(sonora::bench ALIAS sonora_bench)

target_include_directories(sonora_bench PUBLIC
  "${CMAKE_CURRENT_LIST_DIR}/include")

target_link_libraries(sonora_bench PRIVATE nlohmann_json::nlohmann_json)

target_compile_features(sonora_bench PUBLIC cxx_std_20)
