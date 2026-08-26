file(GLOB RPC_SRC CONFIGURE_DEPENDS src/*.cpp src/*/*.cpp)
add_library(rpc STATIC ${RPC_SRC})
target_include_directories(rpc PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(rpc PUBLIC netlib proto_msgs Threads::Threads)
