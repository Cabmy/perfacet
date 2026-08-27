cmake_minimum_required(VERSION 3.16)
project(Perfacet CXX C)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_package(Threads REQUIRED)
add_compile_options(-Wall -Wextra -g)

option(PERFACET_BUILD_TESTS "构建 doctest 套件" ON)
option(PERFACET_BUILD_BENCH "构建 bench 可执行文件" ON)

include(FetchContent)
set(FETCHCONTENT_BASE_DIR "${CMAKE_SOURCE_DIR}/third_party/_fetch")

# ---- 第三方（全部源码编译，不依赖机器预装包）----
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(llhttp
  URL https://github.com/nodejs/llhttp/archive/refs/tags/release/v9.2.1.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(llhttp)

FetchContent_Declare(json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(json)

FetchContent_Declare(httplib
  URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.18.3.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(HTTPLIB_REQUIRE_OPENSSL OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(httplib)

FetchContent_Declare(cli11
  URL https://github.com/CLIUtils/CLI11/archive/refs/tags/v2.4.2.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(CLI11_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(cli11)

FetchContent_Declare(yaml-cpp
  URL https://github.com/jbeder/yaml-cpp/archive/refs/tags/0.8.0.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(YAML_CPP_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(yaml-cpp)

if(PERFACET_BUILD_TESTS)
  FetchContent_Declare(doctest
    URL https://github.com/doctest/doctest/archive/refs/tags/v2.4.11.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(doctest)
endif()

add_subdirectory(netlib)

if(TARGET llhttp_static)
  set(PERFACET_LLHTTP llhttp_static)
elseif(TARGET llhttp)
  set(PERFACET_LLHTTP llhttp)
else()
  message(FATAL_ERROR "llhttp target not found")
endif()

# ---- P1：每层独立 target；pipeline PRIVATE 不链 frontend ----
function(perfacet_lib name)
  add_library(${name} STATIC ${ARGN})
  target_include_directories(${name} PUBLIC ${CMAKE_SOURCE_DIR}/include)
  target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_features(${name} PUBLIC cxx_std_17)
endfunction()

perfacet_lib(perfacet_ir src/ir/ToolKey.cpp src/ir/classify.cpp)
target_link_libraries(perfacet_ir PUBLIC nlohmann_json::nlohmann_json)

perfacet_lib(perfacet_detail src/detail/Random.cpp)
target_include_directories(perfacet_detail PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(perfacet_detail PUBLIC perfacet_ir)

perfacet_lib(perfacet_policy
  src/policy/Taxonomy.cpp
  src/policy/YamlConfig.cpp
  src/policy/YamlIdentityStore.cpp
  src/policy/JsonlGrantStore.cpp)
target_link_libraries(perfacet_policy PUBLIC perfacet_ir perfacet_detail yaml-cpp::yaml-cpp)

perfacet_lib(perfacet_backend src/backend/HttpMcpBackend.cpp src/backend/KeepAliveClient.cpp)
target_link_libraries(perfacet_backend PUBLIC perfacet_ir perfacet_detail netlib httplib::httplib Threads::Threads)

# RankPolicy 认 Catalog：源文件跟 catalog 编进同一 target，避免 policy↔catalog 静态库环
perfacet_lib(perfacet_catalog
  src/catalog/Catalog.cpp
  src/catalog/ToolIndex.cpp
  src/catalog/FacetView.cpp
  src/catalog/IndexRefresher.cpp
  src/policy/RankPolicy.cpp)
target_link_libraries(perfacet_catalog PUBLIC perfacet_backend perfacet_policy)

perfacet_lib(perfacet_observe src/observe/OtlpHttpJsonTracer.cpp)
target_link_libraries(perfacet_observe PUBLIC perfacet_ir perfacet_detail perfacet_policy netlib httplib::httplib)

perfacet_lib(perfacet_audit src/audit/JsonlAuditLog.cpp)
target_link_libraries(perfacet_audit PUBLIC perfacet_ir perfacet_detail netlib)

perfacet_lib(perfacet_govern src/govern/Governor.cpp src/govern/LocalGovernor.cpp)
target_link_libraries(perfacet_govern PUBLIC perfacet_ir perfacet_detail perfacet_policy perfacet_observe netlib)

perfacet_lib(perfacet_health
  src/health/Health.cpp
  src/health/CountCircuit.cpp
  src/health/RetryPolicy.cpp
  src/health/ProbeHealth.cpp)
target_link_libraries(perfacet_health PUBLIC perfacet_ir perfacet_detail perfacet_policy perfacet_catalog netlib)

perfacet_lib(perfacet_task src/task/MemTaskStore.cpp)
target_link_libraries(perfacet_task PUBLIC perfacet_ir)

perfacet_lib(perfacet_pipeline src/pipeline/Pipeline.cpp src/pipeline/InFlight.cpp)
target_link_libraries(perfacet_pipeline PUBLIC
  perfacet_ir perfacet_detail perfacet_policy perfacet_catalog perfacet_backend
  perfacet_govern perfacet_health perfacet_task perfacet_observe perfacet_audit netlib)
# 故意不链 perfacet_frontend

perfacet_lib(perfacet_frontend src/frontend/HttpMcp.cpp)
target_link_libraries(perfacet_frontend PUBLIC perfacet_pipeline perfacet_health perfacet_govern netlib)
target_link_libraries(perfacet_frontend PUBLIC ${PERFACET_LLHTTP})

perfacet_lib(perfacet_cli src/cli/Gateway.cpp src/cli/Cli.cpp)
target_link_libraries(perfacet_cli PUBLIC perfacet_frontend perfacet_observe CLI11::CLI11 httplib::httplib)

add_executable(perfacet src/cli/main.cpp)
target_link_libraries(perfacet PRIVATE perfacet_cli)

add_custom_target(check_layers
  COMMAND ${CMAKE_SOURCE_DIR}/scripts/check_layers.sh
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "分层与语义 grep")
add_dependencies(perfacet check_layers)

if(PERFACET_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(PERFACET_BUILD_BENCH)
  add_subdirectory(bench)
endif()
