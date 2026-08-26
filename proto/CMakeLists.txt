file(GLOB PROTOS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.proto)

# protoc 与 libprotobuf 必须同源同版本。find_program 受 PATH 影响（如 conda 的
# 新版 protoc 抢先命中，而 find_library 找到的是系统 lib），且会缓存在
# CMakeCache 里跨次复用：生成物与运行时不匹配要到编译期才爆炸，
# 这里在配置期直接拦截并给出修法。
if(NOT Protobuf_PROTOC_EXECUTABLE AND TARGET protobuf::protoc)
  get_target_property(Protobuf_PROTOC_EXECUTABLE protobuf::protoc IMPORTED_LOCATION)
endif()
if(NOT Protobuf_PROTOC_EXECUTABLE)
  message(FATAL_ERROR "未找到 protoc 可执行文件（protobuf 安装不完整）")
endif()
execute_process(
  COMMAND ${Protobuf_PROTOC_EXECUTABLE} --version
  OUTPUT_VARIABLE TWIGRPC_PROTOC_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT TWIGRPC_PROTOC_VERSION MATCHES "${Protobuf_VERSION}$")
  message(FATAL_ERROR
    "protoc(${TWIGRPC_PROTOC_VERSION}) 与 libprotobuf(${Protobuf_VERSION}) 版本不一致："
    " PATH 中的 protoc（如 conda/anaconda）与链接的 libprotobuf 不同源。"
    " 修复：deactivate 对应环境后重新 configure，或显式指定同源 protoc："
    " cmake -DProtobuf_PROTOC_EXECUTABLE=/usr/bin/protoc ...")
endif()

# 生成器用 CMake 解析到的绝对路径（与 libprotobuf 同源），不受调用环境 PATH 影响
foreach(P ${PROTOS})
  get_filename_component(NAME ${P} NAME_WE)
  add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/gen/${NAME}.pb.cc ${CMAKE_BINARY_DIR}/gen/${NAME}.pb.h
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/gen.sh ${CMAKE_SOURCE_DIR} ${CMAKE_BINARY_DIR} ${NAME} ${Protobuf_PROTOC_EXECUTABLE}
    DEPENDS ${P} ${CMAKE_CURRENT_SOURCE_DIR}/rpc.proto)
  list(APPEND PB_SRCS ${CMAKE_BINARY_DIR}/gen/${NAME}.pb.cc)
endforeach()
add_library(proto_msgs STATIC ${PB_SRCS})
target_include_directories(proto_msgs PUBLIC ${CMAKE_BINARY_DIR}/gen)
target_link_libraries(proto_msgs PUBLIC protobuf::libprotobuf)
