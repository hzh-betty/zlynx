cmake_minimum_required(VERSION 3.14)
project(include/zhttp VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 查找依赖
find_package(OpenSSL REQUIRED)
find_package(fmt REQUIRED)
find_package(ZLIB REQUIRED)
find_package(nlohmann_json REQUIRED)

find_library(BROTLI_ENC_LIBRARY NAMES brotlienc REQUIRED)

function(zhttp_enable_coverage target_name)
    if(ENABLE_COVERAGE)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(${target_name} PRIVATE -O0 -g --coverage)
            target_link_options(${target_name} PUBLIC --coverage)
        else()
            message(WARNING "ENABLE_COVERAGE is ON, but compiler ${CMAKE_CXX_COMPILER_ID} is not supported")
        endif()
    endif()
endfunction()

# zhttp 库源文件
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

# 创建 zhttp 静态库
add_library(zhttp STATIC ${ZHTTP_SOURCES})
zhttp_enable_coverage(zhttp)

# 头文件目录
target_include_directories(zhttp PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/zlog/include
    ${CMAKE_SOURCE_DIR}/zco/include
    ${CMAKE_SOURCE_DIR}/znet/include
)

# 链接依赖库
target_link_libraries(zhttp PUBLIC
    znet
    zco
    zlog_static
    OpenSSL::SSL
    OpenSSL::Crypto
    fmt::fmt
    ZLIB::ZLIB
    nlohmann_json::nlohmann_json
    pthread
    ${BROTLI_ENC_LIBRARY}
)

# 测试选项
option(BUILD_ZHTTP_TESTS "Build zhttp tests" ON)

if(BUILD_ZHTTP_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
