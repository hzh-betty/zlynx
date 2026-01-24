cmake_minimum_required(VERSION 3.18)

project(zcoroutine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 单配置生成器（Makefile / Ninja）下，未指定时默认 Debug
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# Debug 默认开启 tests，其它默认关闭
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(ENABLE_TESTS_DEFAULT ON)
else()
    set(ENABLE_TESTS_DEFAULT OFF)
endif()

option(ENABLE_TESTS "Enable building tests" ${ENABLE_TESTS_DEFAULT})

file(GLOB ZCOROUTINE_SRCS
        ${PROJECT_SOURCE_DIR}/src/*.cc
)

add_library(zcoroutine_shared SHARED ${ZCOROUTINE_SRCS})
add_library(zcoroutine_static STATIC ${ZCOROUTINE_SRCS})

target_include_directories(zcoroutine_shared
        PUBLIC ${PROJECT_SOURCE_DIR}/include
)

target_include_directories(zcoroutine_static
        PUBLIC ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(zcoroutine_shared
        PUBLIC zlog_shared dl
)

target_link_libraries(zcoroutine_static
        PUBLIC zlog_static dl
)

if(ENABLE_TESTS)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests)
        enable_testing()
        add_subdirectory(tests)
    else()
        message(STATUS "Tests directory not found, skipping tests")
    endif()
endif()

message(STATUS "==============================")
message(STATUS "zcoroutine build configuration")
message(STATUS "------------------------------")
message(STATUS "C++ Standard      : ${CMAKE_CXX_STANDARD}")
message(STATUS "Build Type        : ${CMAKE_BUILD_TYPE}")
message(STATUS "ENABLE_TESTS      : ${ENABLE_TESTS}")
message(STATUS "Shared Library    : zcoroutine_shared")
message(STATUS "Static Library    : zcoroutine_static")
message(STATUS "==============================")
