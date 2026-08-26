file(GLOB NETLIB_SRC CONFIGURE_DEPENDS src/*.cpp)
add_library(netlib STATIC ${NETLIB_SRC})
target_include_directories(netlib PUBLIC include)
target_link_libraries(netlib PUBLIC Threads::Threads)
