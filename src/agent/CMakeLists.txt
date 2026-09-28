# The agent's tool catalogue, the planner interface, and the broker that refuses things.
#
# Portable, and that is the point rather than a convenience: nothing here touches CEF, an
# operating system, a network or a model. The bridge is reached through one std::function the
# caller supplies, so every rule in ADR 0016 -- a tool that is not in the catalogue, a mutation
# that waits for a person, a track title that tries to give instructions -- is a unit test with
# no shell under it.
#
# The same seam as the updater: the part that decides is a library with tests, and something
# else does the acting.

include(GenerateBridge)
sonora_generate_agent_tools(SONORA_AGENT_TOOLS_SOURCE)

find_package(nlohmann_json CONFIG REQUIRED)

add_library(sonora_agent STATIC
  src/broker.cpp
  src/planner.cpp
  src/tools.cpp
  "${SONORA_AGENT_TOOLS_SOURCE}")
add_library(sonora::agent ALIAS sonora_agent)

target_include_directories(sonora_agent PUBLIC
  "${CMAKE_CURRENT_LIST_DIR}/include")

target_link_libraries(sonora_agent PRIVATE nlohmann_json::nlohmann_json)
target_compile_features(sonora_agent PUBLIC cxx_std_20)
