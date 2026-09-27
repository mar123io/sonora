# sonora_release: the release job's packer, compressor and delta generator.
#
# It is here rather than in a script because the archive format, the hash and the patch
# parameters have to exist in exactly one place -- see the comment at the top of
# release_tool.cpp. It is built on every platform, because the release job that runs it
# is not the same machine as the one the release runs on.
add_executable(sonora_release release_tool.cpp)
set_target_properties(sonora_release PROPERTIES OUTPUT_NAME "sonora_release")
target_link_libraries(sonora_release PRIVATE sonora::update)
target_compile_features(sonora_release PRIVATE cxx_std_20)

# sonora_bench: the thing that measures, and the gate that reads what it measured.
#
# The target is sonora_bench_runner and the executable is sonora_bench, because the
# library it links is already called sonora_bench -- and the library is the one that keeps
# the name in the source, since the arithmetic is what other code includes.
#
# It links the library and the scanner, which is the only part of Sonora it measures
# directly; the update metrics it produces by calling into sonora::update the same way the
# release tool does. What it deliberately does not link is anything CEF-shaped: a
# benchmark that needs a window cannot run on a CI runner, and the three metrics that do
# need one are named in ADR 0013 as measured by nothing.
add_executable(sonora_bench_runner bench.cpp)
set_target_properties(sonora_bench_runner PROPERTIES OUTPUT_NAME "sonora_bench")
target_link_libraries(sonora_bench_runner PRIVATE
  sonora::bench
  sonora::library
  sonora::update)
target_compile_features(sonora_bench_runner PRIVATE cxx_std_20)
