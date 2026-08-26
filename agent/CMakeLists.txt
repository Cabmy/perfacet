# libagent：只含 Runtime/TaskTree/PolicyChain（框架核心，不含具体插件）
file(GLOB AGENT_SRC CONFIGURE_DEPENDS src/*.cpp)
add_library(agent STATIC ${AGENT_SRC})
target_include_directories(agent PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(agent PUBLIC rpc)

# libagent_policies：Admission/Retry/NoRetry 插件（可选小库，避免 planner 自抄）
file(GLOB AGENT_POLICY_SRC CONFIGURE_DEPENDS src/policies/*.cpp)
add_library(agent_policies STATIC ${AGENT_POLICY_SRC})
target_include_directories(agent_policies PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(agent_policies PUBLIC agent)
