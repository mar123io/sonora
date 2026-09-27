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
