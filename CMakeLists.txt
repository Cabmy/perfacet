cmake_minimum_required(VERSION 3.16)
project(Perfacet CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_package(Threads REQUIRED)
find_package(Protobuf REQUIRED)
add_compile_options(-Wall -Wextra -g)

# 测试套件是内部设施：产品构建默认不感知 test/ 目录，
# 跑测试时显式开启：cmake -B build -DTWIGRPC_BUILD_TESTS=ON
option(TWIGRPC_BUILD_TESTS "构建测试套件（依赖 Catch2）" OFF)
if(TWIGRPC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()

add_subdirectory(proto)
add_subdirectory(netlib)
add_subdirectory(rpc)
add_subdirectory(agent)
add_subdirectory(examples)
add_subdirectory(benchmark)
add_subdirectory(registryd)
