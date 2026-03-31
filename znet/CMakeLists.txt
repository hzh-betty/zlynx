cmake_minimum_required(VERSION 3.18)
project(znet LANGUAGES CXX)

if(NOT UNIX OR APPLE)
  message(FATAL_ERROR "znet currently supports Linux only.")
endif()

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(ZNET_BUILD_TESTS "Build znet tests" ON)

if(NOT TARGET zcoroutine)
  set(ZCOROUTINE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(ZCOROUTINE_ENABLE_LOGGING ON CACHE BOOL "" FORCE)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../zcoroutine ${CMAKE_BINARY_DIR}/zcoroutine)
endif()

add_library(znet STATIC
  src/address.cc
  src/acceptor.cc
  src/buffer.cc
  src/server.cc
  src/session.cc
  src/socket.cc
  src/tcp_connection.cc
  src/tcp_server.cc
  src/znet_logger.cc
)

target_include_directories(znet
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(znet
  PUBLIC
    zcoroutine
)

target_compile_options(znet PRIVATE
  -Wall
  -Wextra
  -Wpedantic
)

if(ZNET_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
