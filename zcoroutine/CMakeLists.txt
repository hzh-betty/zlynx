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

# 工具类
file(GLOB UTIL_SRCS
        ${PROJECT_SOURCE_DIR}/src/util/*.cc
)

# 同步原语
file(GLOB SYNC_SRCS
        ${PROJECT_SOURCE_DIR}/src/sync/*.cc
)

# 运行时
file(GLOB RUNTIME_SRCS
        ${PROJECT_SOURCE_DIR}/src/runtime/*.cc
)

# 调度层
file(GLOB SCHEDULING_SRCS
        ${PROJECT_SOURCE_DIR}/src/scheduling/*.cc
)

# IO 层
file(GLOB IO_SRCS
        ${PROJECT_SOURCE_DIR}/src/io/*.cc
)

# 定时器
file(GLOB TIMER_SRCS
        ${PROJECT_SOURCE_DIR}/src/timer/*.cc
)

# Hook 层
file(GLOB HOOK_SRCS
        ${PROJECT_SOURCE_DIR}/src/hook/*.cc
)

set(ZCOROUTINE_SRCS
        ${UTIL_SRCS}
        ${SYNC_SRCS}
        ${RUNTIME_SRCS}
        ${SCHEDULING_SRCS}
        ${IO_SRCS}
        ${TIMER_SRCS}
        ${HOOK_SRCS}
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
