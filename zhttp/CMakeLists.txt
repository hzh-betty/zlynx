cmake_minimum_required(VERSION 3.18)

project(zhttp VERSION 4.0.0 LANGUAGES CXX)

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
    find_package(znet 4 CONFIG REQUIRED)
    set(ZHTTP_ZNET_TARGET znet::znet)
endif()
if(TARGET zlog)
    set(ZHTTP_ZLOG_TARGET zlog)
else()
    find_package(zlog CONFIG REQUIRED)
    set(ZHTTP_ZLOG_TARGET zlog::zlog)
endif()

set(ZHTTP_CORE_SOURCES
    src/application/http_application.cc
    src/content/multipart.cc
    src/content/parsed_request_body.cc
    src/message/http_body.cc
    src/message/http_common.cc
    src/message/http_context.cc
    src/message/http_headers.cc
    src/message/http_request.cc
    src/message/http_response.cc
    src/static_files/path.cc
    src/router/radix_tree.cc
    src/static_files/range.cc
    src/middleware/auth_middleware.cc
    src/middleware/compression_middleware.cc
    src/middleware/cors_middleware.cc
    src/middleware/error_middleware.cc
    src/rate_limit/rate_limiter.cc
    src/middleware/rate_limiter_middleware.cc
    src/middleware/request_body_middleware.cc
    src/middleware/security_middleware.cc
    src/middleware/session_middleware.cc
    src/middleware/static_file_middleware.cc
    src/middleware/timeout_middleware.cc
    src/application/request_pipeline.cc
    src/router/router.cc
    src/static_files/static_resource_store.cc
    src/session/session.cc
    src/message/uri.cc
    src/protocol/http/response_encoder.cc
)
set(ZHTTP_SOURCES
    src/runtime/http_server.cc
    src/runtime/http_server_builder.cc
    src/protocol/http/chunked_decoder.cc
    src/protocol/http/http_request_parser.cc
    src/protocol/websocket/websocket_frame_parser.cc
    src/protocol/connection_session.cc
    src/protocol/http/http_protocol_handler.cc
    src/protocol/websocket/websocket_protocol_handler.cc
    src/runtime/daemon.cc
    src/runtime/server_bootstrap.cc
    src/runtime/server_runner.cc
    src/config/server_config.cc
    src/protocol/websocket/websocket_connection.cc
    src/protocol/websocket/websocket_frame.cc
    src/protocol/websocket/websocket_handshake.cc
    src/protocol/websocket/websocket_message_assembler.cc
    src/protocol/http/http_response_writer.cc
    src/runtime/logging.cc
)
add_library(zhttp_core ${ZHTTP_CORE_SOURCES})
add_library(zhttp::core ALIAS zhttp_core)
add_library(zhttp ${ZHTTP_SOURCES})
add_library(zhttp::zhttp ALIAS zhttp)
foreach(target zhttp_core zhttp)
    target_include_directories(${target} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    zlynx_apply_common_options(${target})
endforeach()
target_link_libraries(zhttp_core PUBLIC nlohmann_json::nlohmann_json
    PRIVATE ZLIB::ZLIB ${BROTLI_ENC_LIBRARY})
target_link_libraries(zhttp PUBLIC zhttp_core ${ZHTTP_ZNET_TARGET}
    PRIVATE ${ZHTTP_ZLOG_TARGET} OpenSSL::SSL OpenSSL::Crypto fmt::fmt)
set_target_properties(zhttp_core PROPERTIES EXPORT_NAME core
    VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR})
zlynx_install_extra_target(zhttp zhttp_core)

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
    DEPENDENCIES "znet 4" zlog OpenSSL fmt ZLIB nlohmann_json
)
