cmake_minimum_required(VERSION 3.18)

project(znet VERSION 1.0.0 LANGUAGES CXX)

# znet 支持源码树内构建和独立 find_package 构建。
# 独立构建时需要加载公共 helper，并从安装前缀查找 zco。
if(NOT COMMAND zlynx_apply_common_options)
    set(ZLYNX_CMAKE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")
    list(APPEND CMAKE_MODULE_PATH "${ZLYNX_CMAKE_DIR}")
    include(ZlynxOptions)
    include(ZlynxPackage)
    include(ZlynxTesting)
endif()

if(NOT UNIX OR APPLE)
    message(FATAL_ERROR "znet currently supports Linux only.")
endif()

find_package(OpenSSL REQUIRED)

# 源码树内链接本地 zco target；安装包/独立构建时链接 zco::zco。
if(TARGET zco)
    set(ZNET_ZCO_TARGET zco)
else()
    find_package(zco CONFIG REQUIRED)
    set(ZNET_ZCO_TARGET zco::zco)
endif()

add_library(znet
    src/address.cc
    src/acceptor.cc
    src/buffer.cc
    src/socket.cc
    src/tcp_connection.cc
    src/tcp_server.cc
    src/tls_context.cc
    src/znet_logger.cc
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
    DEPENDENCIES zco OpenSSL
)
