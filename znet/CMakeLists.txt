cmake_minimum_required(VERSION 3.18)

project(znet VERSION 4.0.0 LANGUAGES CXX)

# znet 支持源码树内构建和独立 find_package 构建。
# 独立构建时需要加载公共 helper，并从安装前缀查找 zco。
if(NOT COMMAND zlynx_apply_common_options)
    set(ZLYNX_CMAKE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")
    list(APPEND CMAKE_MODULE_PATH "${ZLYNX_CMAKE_DIR}")
    include(ZlynxOptions)
    include(ZlynxPackage)
    include(ZlynxTesting)
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "znet currently supports Linux only.")
endif()

find_package(OpenSSL 1.1 REQUIRED)

# 源码树内链接本地 zco target；安装包/独立构建时链接 zco::zco。
if(TARGET zco)
    set(ZNET_ZCO_TARGET zco)
else()
    find_package(zco 3 CONFIG REQUIRED)
    set(ZNET_ZCO_TARGET zco::zco)
endif()

add_library(znet
    src/error.cc
    src/endpoint.cc
    src/byte_buffer.cc
    src/transport/socket.cc
    src/transport/tcp_stream.cc
    src/transport/tls_stream.cc
    src/server/connection.cc
    src/server/tcp_server.cc
)
add_library(znet::znet ALIAS znet)

# znet 头文件被 zhttp 和外部消费者包含，include 目录必须作为 PUBLIC 用法需求。
target_include_directories(znet
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(znet
    PUBLIC
        ${ZNET_ZCO_TARGET}
    PRIVATE
        OpenSSL::SSL
        OpenSSL::Crypto
)
zlynx_apply_common_options(znet)

# 安装导出为 znet::znet，并带版本化 soname。
set_target_properties(znet PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    EXPORT_NAME znet
)

# znet 的 wrk benchmark 位于 tests/benchmark，perf 构建也需要进入 tests。
if(BUILD_TESTING OR ZLYNX_BUILD_PERF_TESTS)
    add_subdirectory(tests)
endif()

zlynx_install_module(znet znet
    VERSION ${PROJECT_VERSION}
    DEPENDENCIES "zco 3" OpenSSL
)
