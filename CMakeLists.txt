cmake_minimum_required(VERSION 3.18)

project(zlynx VERSION 1.0.0 LANGUAGES CXX)

# 默认 C++14 标准（子项目可覆盖）
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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
option(BUILD_SHARED_LIBS "Build shared libraries" ON)

# 查找公共依赖
find_package(Threads REQUIRED)

# 子模块构建顺序（按依赖关系）
# zmalloc: 无依赖，最底层
# zlog: 无依赖
# zcoroutine: 依赖 zlog
# znet: 依赖 zcoroutine, zlog
# zhttp: 依赖 znet, zcoroutine, zlog

add_subdirectory(zmalloc)
add_subdirectory(zlog)
add_subdirectory(zcoroutine)
add_subdirectory(znet)
add_subdirectory(zhttp)

# 输出配置信息
message(STATUS "==============================")
message(STATUS "zlynx project configuration")
message(STATUS "------------------------------")
message(STATUS "C++ Standard      : ${CMAKE_CXX_STANDARD}")
message(STATUS "Build Type        : ${CMAKE_BUILD_TYPE}")
message(STATUS "ENABLE_TESTS      : ${ENABLE_TESTS}")
message(STATUS "BUILD_SHARED_LIBS : ${BUILD_SHARED_LIBS}")
message(STATUS "")
message(STATUS "Submodules:")
message(STATUS "  - zmalloc")
message(STATUS "  - zlog")
message(STATUS "  - zcoroutine")
message(STATUS "  - znet")
message(STATUS "  - zhttp")
message(STATUS "==============================")
