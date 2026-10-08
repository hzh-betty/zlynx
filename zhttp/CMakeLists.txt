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
    content/multipart.cc
    content/parsed_request_body.cc
    http_body.cc
    http_common.cc
    http_context.cc
    http_headers.cc
    http_request.cc
    http_response.cc
    http_server.cc
    http_server_builder.cc
    internal/http_utils.cc
    internal/radix_tree.cc
    internal/range_parse.cc
    middleware/auth_middleware.cc
    middleware/compression_middleware.cc
    middleware/cors_middleware.cc
    middleware/error_middleware.cc
    middleware/rate_limiter_middleware.cc
    middleware/request_body_middleware.cc
    middleware/security_middleware.cc
    middleware/session_middleware.cc
    middleware/static_file_middleware.cc
    middleware/timeout_middleware.cc
    parser/chunked_decoder.cc
    parser/http_request_parser.cc
    parser/websocket_frame_parser.cc
    pipeline/request_pipeline.cc
    protocol/http_protocol_handler.cc
    protocol/websocket_protocol_handler.cc
    router/router.cc
    runtime/daemon.cc
    runtime/server_bootstrap.cc
    runtime/server_runner.cc
    server_config.cc
    session.cc
    uri.cc
    websocket/websocket_connection.cc
    websocket/websocket_frame.cc
    websocket/websocket_handshake.cc
    websocket/websocket_message_assembler.cc
    writer/http_response_writer.cc
    zhttp_logger.cc
)

add_library(zhttp ${ZHTTP_SOURCES})
add_library(zhttp::zhttp ALIAS zhttp)

# zhttp public headers 直接包含 znet/zlog/nlohmann_json 类型，
# 因此这些依赖需要通过 PUBLIC 链接传播给消费者。
target_include_directories(zhttp
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
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
    PUBLIC_HEADERS
        http_body.h
        http_common.h
        http_context.h
        http_headers.h
        http_request.h
        http_response.h
        http_server.h
        http_server_builder.h
        request_limits.h
        server_config.h
        session.h
        uri.h
        zhttp.h
        zhttp_logger.h
        router/route_handler.h
        router/router.h
        middleware/auth_middleware.h
        middleware/compression_middleware.h
        middleware/cors_middleware.h
        middleware/error_middleware.h
        middleware/middleware.h
        middleware/rate_limiter_middleware.h
        middleware/request_body_middleware.h
        middleware/security_middleware.h
        middleware/session_middleware.h
        middleware/static_file_middleware.h
        middleware/timeout_middleware.h
        content/parsed_request_body.h
        content/multipart.h
        runtime/daemon.h
        websocket/websocket_connection.h
        websocket/websocket_handler.h
        websocket/websocket_types.h
    DEPENDENCIES znet zlog OpenSSL fmt ZLIB nlohmann_json
)
