cmake_minimum_required(VERSION 3.16)
file(GLOB_RECURSE PERFACET_TEST_SRC CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
add_executable(perfacet_tests ${PERFACET_TEST_SRC})
target_link_libraries(perfacet_tests PRIVATE perfacet_pipeline perfacet_policy perfacet_frontend doctest::doctest)
target_include_directories(perfacet_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME perfacet_tests COMMAND perfacet_tests)
set_tests_properties(perfacet_tests PROPERTIES TIMEOUT 300)
add_test(NAME check_layers
  COMMAND ${CMAKE_SOURCE_DIR}/scripts/check_layers.sh
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
