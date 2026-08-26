cmake_minimum_required(VERSION 3.16)
project(grpc_echo CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 独立于主项目构建（对照基准）：链接 anaconda 的 grpc 1.71 + protobuf 29.3
# 用法：cmake -DGRPC_ROOT=/home/cabmy/anaconda3 -B build && cmake --build build
set(GRPC_ROOT "/home/cabmy/anaconda3" CACHE PATH "grpc root (anaconda)")
set(ENV{PKG_CONFIG_PATH} "${GRPC_ROOT}/lib/pkgconfig")

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GRPC++ REQUIRED grpc++)
pkg_check_modules(PROTOBUF REQUIRED protobuf)

add_executable(echo_server echo_server.cpp gen/echo.grpc.pb.cc gen/echo.pb.cc)
add_executable(echo_client echo_client.cpp gen/echo.grpc.pb.cc gen/echo.pb.cc)
foreach(tgt echo_server echo_client)
  target_include_directories(${tgt} PRIVATE ${GRPC++_INCLUDE_DIRS} ${PROTOBUF_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR}/gen ${CMAKE_CURRENT_SOURCE_DIR}/../..)
  target_link_directories(${tgt} PRIVATE ${GRPC++_LIBRARY_DIRS} ${PROTOBUF_LIBRARY_DIRS})
  target_link_libraries(${tgt} PRIVATE ${GRPC++_LIBRARIES} ${PROTOBUF_LIBRARIES} Threads::Threads)
endforeach()
