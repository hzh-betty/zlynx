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
option(ENABLE_COVERAGE "Enable code coverage instrumentation" OFF)
option(ZLYNX_USE_ZMALLOC_OVERRIDE "Use zmalloc as the project-wide allocator override" ON)

# 查找公共依赖
find_package(Threads REQUIRED)

# 子模块构建顺序（按依赖关系）
# zmalloc: 无依赖，最底层
# zlog: 无依赖
# zco: 依赖 zlog
# znet: 依赖 zco, zlog
# zhttp: 依赖 znet, zco, zlog
enable_testing()

add_subdirectory(zmalloc)
add_subdirectory(zlog)
add_subdirectory(zco)
add_subdirectory(znet)
add_subdirectory(zhttp)

if(ZLYNX_USE_ZMALLOC_OVERRIDE)
    if(TARGET zlog_shared)
        target_link_libraries(zlog_shared PRIVATE zmalloc_override)
    endif()
    if(TARGET zlog_static)
        target_link_libraries(zlog_static PUBLIC zmalloc_override)
    endif()
    if(TARGET zco)
        target_link_libraries(zco PUBLIC zmalloc_override)
    endif()
    if(TARGET znet)
        target_link_libraries(znet PUBLIC zmalloc_override)
    endif()
    if(TARGET zhttp)
        target_link_libraries(zhttp PUBLIC zmalloc_override)
    endif()
endif()

# 输出配置信息
message(STATUS "==============================")
message(STATUS "zlynx project configuration")
message(STATUS "------------------------------")
message(STATUS "C++ Standard      : ${CMAKE_CXX_STANDARD}")
message(STATUS "Build Type        : ${CMAKE_BUILD_TYPE}")
message(STATUS "ENABLE_TESTS      : ${ENABLE_TESTS}")
message(STATUS "BUILD_SHARED_LIBS : ${BUILD_SHARED_LIBS}")
message(STATUS "ENABLE_COVERAGE   : ${ENABLE_COVERAGE}")
message(STATUS "ZMALLOC_OVERRIDE  : ${ZLYNX_USE_ZMALLOC_OVERRIDE}")
message(STATUS "")
message(STATUS "Submodules:")
message(STATUS "  - zmalloc")
message(STATUS "  - zlog")
message(STATUS "  - zco")
message(STATUS "  - znet")
message(STATUS "  - zhttp")
message(STATUS "==============================")
