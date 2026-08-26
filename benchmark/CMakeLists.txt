add_executable(bench bench.cpp)
target_include_directories(bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bench PRIVATE rpc)
