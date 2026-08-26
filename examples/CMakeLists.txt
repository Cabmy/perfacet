add_executable(echo_netlib echo_netlib.cpp)
target_link_libraries(echo_netlib PRIVATE netlib)

add_executable(math_server math_server.cpp)
target_link_libraries(math_server PRIVATE rpc)

add_executable(math_client math_client.cpp)
target_link_libraries(math_client PRIVATE rpc)

add_executable(discovery_client discovery_client.cpp)
target_link_libraries(discovery_client PRIVATE rpc)

add_executable(kv_server kv_server.cpp)
target_link_libraries(kv_server PRIVATE rpc)

add_executable(local_mux_server local_mux_server.cpp)
target_link_libraries(local_mux_server PRIVATE rpc)

add_executable(local_mux_planner local_mux_planner.cpp)
target_link_libraries(local_mux_planner PRIVATE rpc)

add_executable(agent_demo agent_demo.cpp)
target_link_libraries(agent_demo PRIVATE agent agent_policies)
