cmake_minimum_required(VERSION 3.18)

project(zlog VERSION 1.0.0 LANGUAGES CXX)

# 支持两种使用方式：
# 1. 从仓库根目录 add_subdirectory(zlog)，公共 helper 已由根 CMakeLists 加载；
# 2. 单独配置 zlog/ 目录，下面的 fallback 会加载 ../cmake 中的公共函数。
if(NOT COMMAND zlynx_apply_common_options)
    set(ZLYNX_CMAKE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")
    list(APPEND CMAKE_MODULE_PATH "${ZLYNX_CMAKE_DIR}")
    include(ZlynxOptions)
    include(ZlynxPackage)
    include(ZlynxTesting)
endif()

find_package(fmt REQUIRED CONFIG)
find_package(Threads REQUIRED)

# zlog 源文件显式列出，避免 GLOB 在发布包/安装包中引入不确定性。
set(ZLOG_SOURCES
    src/buffer.cc
    src/format.cc
    src/level.cc
    src/logger.cc
    src/looper.cc
    src/message.cc
    src/sink.cc
    src/util.cc
)

add_library(zlog ${ZLOG_SOURCES})
add_library(zlog::zlog ALIAS zlog)

# PUBLIC include 同时区分构建树和安装树：
# - BUILD_INTERFACE 使用源码目录 include；
# - INSTALL_INTERFACE 使用安装后的 include 前缀。
target_include_directories(zlog
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(zlog
    PUBLIC
        fmt::fmt
        Threads::Threads
)
# 统一注入 C++ 标准、warning 和可选 coverage；这些策略由 helper 控制。
zlynx_apply_common_options(zlog)

# EXPORT_NAME 决定 install/export 后的目标名为 zlog::zlog。
set_target_properties(zlog PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    EXPORT_NAME zlog
)

# 普通测试由 BUILD_TESTING 控制；perf 目标需要 tests/benchmark 下的源码，
# 所以 ZLYNX_BUILD_PERF_TESTS 打开时也进入 tests 子目录，但不会注册 CTest。
if(BUILD_TESTING OR ZLYNX_BUILD_PERF_TESTS)
    add_subdirectory(tests)
endif()

zlynx_install_module(zlog zlog
    VERSION ${PROJECT_VERSION}
    DEPENDENCIES fmt Threads
)
