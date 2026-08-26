cmake_minimum_required(VERSION 3.16)
project(Perfacet CXX C)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_package(Threads REQUIRED)
add_compile_options(-Wall -Wextra -g)

option(PERFACET_BUILD_TESTS "构建 doctest 套件" ON)

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

file(GLOB_RECURSE PERFACET_SRC CONFIGURE_DEPENDS
  src/ir/*.cpp
  src/pipeline/*.cpp
  src/catalog/*.cpp
  src/govern/*.cpp
  src/policy/*.cpp
  src/health/*.cpp
  src/backend/*.cpp
  src/task/*.cpp
  src/observe/*.cpp
  src/frontend/*.cpp
  src/audit/*.cpp
  src/cli/*.cpp
  src/detail/*.cpp)
list(FILTER PERFACET_SRC EXCLUDE REGEX "src/cli/main.cpp$")

add_library(perfacet_core STATIC ${PERFACET_SRC})
target_include_directories(perfacet_core PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(perfacet_core PUBLIC
  netlib
  nlohmann_json::nlohmann_json
  yaml-cpp::yaml-cpp
  httplib::httplib
  CLI11::CLI11
  Threads::Threads)
# llhttp 目标名随版本可能是 llhttp_static / llhttp
if(TARGET llhttp_static)
  target_link_libraries(perfacet_core PUBLIC llhttp_static)
elseif(TARGET llhttp)
  target_link_libraries(perfacet_core PUBLIC llhttp)
else()
  message(FATAL_ERROR "llhttp target not found")
endif()

add_executable(perfacet src/cli/main.cpp)
target_link_libraries(perfacet PRIVATE perfacet_core)

add_custom_target(check_layers
  COMMAND ${CMAKE_SOURCE_DIR}/scripts/check_layers.sh
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "分层与语义 grep")
add_dependencies(perfacet check_layers)

if(PERFACET_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
