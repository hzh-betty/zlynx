cmake_minimum_required(VERSION 3.14)
project(zhttp VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 查找依赖
find_package(OpenSSL REQUIRED)
find_package(fmt REQUIRED)

# zhttp 库源文件
set(ZHTTP_SOURCES
    src/zhttp_logger.cc
    src/allocator.cc
    src/http_common.cc
    src/http_request.cc
    src/http_response.cc
    src/http_parser.cc
    src/middleware.cc
    src/radix_tree.cc
    src/router.cc
    src/http_server.cc
    src/ssl_context.cc
    src/https_server.cc
    src/daemon.cc
    src/server_config.cc
    src/http_server_builder.cc
    src/uri.cc
)

# 创建 zhttp 静态库
add_library(zhttp STATIC ${ZHTTP_SOURCES})

# 头文件目录
target_include_directories(zhttp PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/zlog/include
    ${CMAKE_SOURCE_DIR}/zcoroutine/include
    ${CMAKE_SOURCE_DIR}/znet/include
)

# 链接依赖库
target_link_libraries(zhttp PUBLIC
    znet_static
    zcoroutine_static
    zlog_static
    OpenSSL::SSL
    OpenSSL::Crypto
    fmt::fmt
    pthread
)

# 测试选项
option(BUILD_ZHTTP_TESTS "Build zhttp tests" ON)

if(BUILD_ZHTTP_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
