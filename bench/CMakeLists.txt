cmake_minimum_required(VERSION 3.16)
add_executable(perfacet_bench overhead.cpp)
target_link_libraries(perfacet_bench PRIVATE perfacet_pipeline)
target_include_directories(perfacet_bench PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/tests)
