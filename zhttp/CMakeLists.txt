cmake_minimum_required(VERSION 3.18)

project(zhttp VERSION 1.0.0 LANGUAGES CXX)

# zhttp 是最上层 HTTP/WebSocket 模块。
# 根工程构建时依赖本地 znet/zlog target；独立构建时通过 find_package 获取。
if(NOT COMMAND zlynx_apply_common_options)
    set(ZLYNX_CMAKE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")
    list(APPEND CMAKE_MODULE_PATH "${ZLYNX_CMAKE_DIR}")
    include(ZlynxOptions)
    include(ZlynxPackage)
    include(ZlynxTesting)
endif()

find_package(OpenSSL REQUIRED)
find_package(fmt REQUIRED CONFIG)
find_package(ZLIB REQUIRED)
find_package(nlohmann_json REQUIRED CONFIG)
find_library(BROTLI_ENC_LIBRARY NAMES brotlienc REQUIRED)

# 同时兼容源码树内 target 和安装后的命名空间 target。
if(TARGET znet)
    set(ZHTTP_ZNET_TARGET znet)
else()
    find_package(znet CONFIG REQUIRED)
    set(ZHTTP_ZNET_TARGET znet::znet)
endif()
if(TARGET zlog)
    set(ZHTTP_ZLOG_TARGET zlog)
else()
    find_package(zlog CONFIG REQUIRED)
    set(ZHTTP_ZLOG_TARGET zlog::zlog)
endif()

set(ZHTTP_SOURCES
    src/zhttp_logger.cc
    src/http_common.cc
    src/http_request.cc
    src/http_response.cc
    src/http_parser.cc
    src/multipart.cc
    src/radix_tree.cc
    src/router.cc
    src/http_utils.cc
    src/range_parse.cc
    src/mid/middleware.cc
    src/mid/rate_limiter_middleware.cc
    src/mid/static_file_middleware.cc
    src/mid/auth_middleware.cc
    src/mid/cors_middleware.cc
    src/mid/compression_middleware.cc
    src/mid/error_middleware.cc
    src/mid/request_body_middleware.cc
    src/mid/timeout_middleware.cc
    src/mid/security_middleware.cc
    src/mid/session_middleware.cc
    src/http_server.cc
    src/websocket_frame.cc
    src/websocket.cc
    src/session.cc
    src/daemon.cc
    src/server_config.cc
    src/http_server_builder.cc
)

add_library(zhttp ${ZHTTP_SOURCES})
add_library(zhttp::zhttp ALIAS zhttp)

# zhttp public headers 直接包含 znet/zlog/nlohmann_json 类型，
# 因此这些依赖需要通过 PUBLIC 链接传播给消费者。
target_include_directories(zhttp
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(zhttp
    PUBLIC
        ${ZHTTP_ZNET_TARGET}
        ${ZHTTP_ZLOG_TARGET}
        nlohmann_json::nlohmann_json
    PRIVATE
        OpenSSL::SSL
        OpenSSL::Crypto
        fmt::fmt
        ZLIB::ZLIB
        ${BROTLI_ENC_LIBRARY}
)
zlynx_apply_common_options(zhttp)

# 安装导出为 zhttp::zhttp；版本号用于生成 ConfigVersion 文件。
set_target_properties(zhttp PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    EXPORT_NAME zhttp
)

# benchmark 位于 tests/benchmark；只构建 perf 时也进入 tests，但不注册 CTest。
if(BUILD_TESTING OR ZLYNX_BUILD_PERF_TESTS)
    add_subdirectory(tests)
endif()

zlynx_install_module(zhttp zhttp
    VERSION ${PROJECT_VERSION}
    DEPENDENCIES znet zlog OpenSSL fmt ZLIB nlohmann_json
)
