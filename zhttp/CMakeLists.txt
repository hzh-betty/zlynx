cmake_minimum_required(VERSION 3.18)
project(zhttp VERSION 1.0.0 LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    include(CTest)
endif()
option(BUILD_BENCHMARKS "Build benchmark executables" OFF)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "zhttp currently supports Linux only")
endif()

if(NOT TARGET zco::zco)
    find_package(zco 1 CONFIG REQUIRED)
endif()
if(NOT TARGET znet::znet)
    find_package(znet 1 CONFIG REQUIRED)
endif()
if(NOT TARGET zlog::zlog)
    find_package(zlog 1 CONFIG REQUIRED)
endif()
find_package(fmt CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(toml11 CONFIG REQUIRED)
find_package(OpenSSL 1.1 REQUIRED COMPONENTS Crypto)
find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(Brotli REQUIRED IMPORTED_TARGET libbrotlienc)

add_library(zhttp
    src/application/http_application.cc
    src/application/request_pipeline.cc
    src/config/server_config.cc
    src/content/multipart.cc
    src/content/parsed_request_body.cc
    src/message/http_body.cc
    src/message/http_common.cc
    src/message/http_context.cc
    src/message/http_headers.cc
    src/message/http_request.cc
    src/message/http_response.cc
    src/message/uri.cc
    src/middleware/auth_middleware.cc
    src/middleware/compression_middleware.cc
    src/middleware/cors_middleware.cc
    src/middleware/error_middleware.cc
    src/middleware/rate_limiter_middleware.cc
    src/middleware/request_body_middleware.cc
    src/middleware/security_middleware.cc
    src/middleware/session_middleware.cc
    src/middleware/static_file_middleware.cc
    src/middleware/timeout_middleware.cc
    src/protocol/connection_session.cc
    src/protocol/http/chunked_decoder.cc
    src/protocol/http/http_protocol_handler.cc
    src/protocol/http/http_request_parser.cc
    src/protocol/http/http_response_writer.cc
    src/protocol/http/response_encoder.cc
    src/protocol/websocket/websocket_connection.cc
    src/protocol/websocket/websocket_frame.cc
    src/protocol/websocket/websocket_frame_parser.cc
    src/protocol/websocket/websocket_handshake.cc
    src/protocol/websocket/websocket_message_assembler.cc
    src/protocol/websocket/websocket_protocol_handler.cc
    src/rate_limit/rate_limiter.cc
    src/router/radix_tree.cc
    src/router/router.cc
    src/runtime/daemon.cc
    src/runtime/http_server.cc
    src/runtime/http_server_builder.cc
    src/runtime/logging.cc
    src/runtime/server_bootstrap.cc
    src/runtime/server_runner.cc
    src/session/session.cc
    src/static_files/path.cc
    src/static_files/range.cc
    src/static_files/static_resource_store.cc
)
add_library(zhttp::zhttp ALIAS zhttp)

set(zhttp_cxx_standard 17)
target_compile_features(zhttp PUBLIC cxx_std_${zhttp_cxx_standard})
if(NOT CMAKE_CXX_STANDARD)
    set_property(TARGET zhttp PROPERTY CXX_STANDARD ${zhttp_cxx_standard})
endif()
set_target_properties(zhttp PROPERTIES
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})
target_compile_options(zhttp PRIVATE
    "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>"
    "$<$<CXX_COMPILER_ID:MSVC>:/W4;/permissive->")
target_include_directories(zhttp
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(zhttp
    PUBLIC znet::znet zco::zco nlohmann_json::nlohmann_json
    PRIVATE zlog::zlog fmt::fmt toml11::toml11 OpenSSL::Crypto ZLIB::ZLIB PkgConfig::Brotli Threads::Threads)

if(BUILD_TESTING OR BUILD_BENCHMARKS)
    add_subdirectory(tests)
endif()

install(TARGETS zhttp EXPORT zhttpTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT zhttpTargets NAMESPACE zhttp::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/zhttp)
configure_package_config_file(cmake/zhttpConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/zhttpConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/zhttp)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/zhttpConfigVersion.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/zhttpConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/zhttpConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/zhttp)
