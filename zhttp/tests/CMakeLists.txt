find_package(GTest REQUIRED)
find_library(BROTLI_DEC_LIBRARY NAMES brotlidec REQUIRED)

function(zhttp_test_enable_coverage target_name)
    if(ENABLE_COVERAGE)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(${target_name} PRIVATE -O0 -g --coverage)
            target_link_options(${target_name} PRIVATE --coverage)
        endif()
    endif()
endfunction()

# 单元测试列表
set(UNIT_TESTS
    http_parser_test
    http_common_test
    http_request_test
    http_response_test
    http_utils_test
    websocket_frame_test
    websocket_handshake_test
    multipart_test
    server_config_test
    session_test
    rate_limiter_test
    router_test
    middleware_test
    error_middleware_test
    auth_middleware_test
    cors_middleware_test
    compression_middleware_test
    request_body_middleware_test
    timeout_middleware_test
    security_middleware_test
    static_file_opt_test
    http_server_context_test
    router_detailed_test
    regex_prefix_bucket_test
    range_parse_test
    zhttp_logger_test
    daemon_test
    http_server_builder_test
)

# 为每个单元测试创建可执行文件
foreach(test_name ${UNIT_TESTS})
    add_executable(${test_name} unit/${test_name}.cc)
    target_link_libraries(${test_name}
        zhttp
        znet
        zco
        zlog_static
        GTest::gtest
        pthread
    )
    target_include_directories(${test_name} PRIVATE
        ${CMAKE_SOURCE_DIR}/zhttp/include
    )
    zhttp_test_enable_coverage(${test_name})
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# These tests manipulate process-wide state (signals/fork/runtime internals)
# and are more stable when isolated from high parallel pressure.
foreach(test_name http_server_context_test daemon_test)
    if(TARGET ${test_name})
        set_tests_properties(${test_name} PROPERTIES
            RUN_SERIAL TRUE
            RESOURCE_LOCK zco_runtime
        )
    endif()
endforeach()

target_link_libraries(compression_middleware_test
    ${BROTLI_DEC_LIBRARY}
)

# 集成测试
add_executable(http_server_test integration/http_server_test.cc)
target_link_libraries(http_server_test
    zhttp
    znet
    zco
    zlog_static
    GTest::gtest
    pthread
)
target_include_directories(http_server_test PRIVATE
    ${CMAKE_SOURCE_DIR}/zhttp/include
)
zhttp_test_enable_coverage(http_server_test)
add_test(NAME http_server_test COMMAND http_server_test)

add_executable(websocket_server_test integration/websocket_server_test.cc)
target_link_libraries(websocket_server_test
    zhttp
    znet
    zco
    zlog_static
    GTest::gtest
    pthread
)
target_include_directories(websocket_server_test PRIVATE
    ${CMAKE_SOURCE_DIR}/zhttp/include
)
zhttp_test_enable_coverage(websocket_server_test)
add_test(NAME websocket_server_test COMMAND websocket_server_test)

add_executable(zhttp_benchmark benchmark/zhttp_benchmark.cc)

target_link_libraries(zhttp_benchmark
    zhttp
    znet
    zco
    zlog_static
    pthread
)

target_include_directories(zhttp_benchmark PRIVATE
    ${CMAKE_SOURCE_DIR}/zhttp/include
)
zhttp_test_enable_coverage(zhttp_benchmark)
