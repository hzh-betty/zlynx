cmake_minimum_required(VERSION 3.18)

find_package(GTest REQUIRED)
find_package(Threads REQUIRED)
find_library(BROTLI_DEC_LIBRARY NAMES brotlidec REQUIRED)

# 单元测试显式列表便于对个别目标附加额外依赖或串行属性。
set(ZHTTP_UNIT_TESTS
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

if(BUILD_TESTING)
    # unit/integration 进入 CTest；stress/perf 不进入 CTest。
    foreach(test_name ${ZHTTP_UNIT_TESTS})
        zlynx_add_gtest(${test_name} unit/${test_name}.cc zhttp unit zhttp)
    endforeach()

    target_link_libraries(compression_middleware_test PRIVATE ZLIB::ZLIB ${BROTLI_DEC_LIBRARY})

    foreach(test_name http_server_context_test daemon_test)
        if(TARGET ${test_name})
            # 这些测试涉及进程/运行时全局状态，串行执行更稳定。
            set_tests_properties(zhttp.unit.${test_name} PROPERTIES
                RUN_SERIAL TRUE
                RESOURCE_LOCK zco_runtime
            )
        endif()
    endforeach()

    set(ZHTTP_INTEGRATION_TESTS
        http_server_test
        websocket_server_test
    )

    foreach(test_name ${ZHTTP_INTEGRATION_TESTS})
        zlynx_add_gtest(${test_name} integration/${test_name}.cc zhttp integration zhttp)
    endforeach()
endif()

if(ZLYNX_BUILD_PERF_TESTS)
    # HTTP benchmark 需要手动运行，不参与覆盖率和 CTest。
    zlynx_add_perf_target(zhttp_benchmark benchmark/zhttp_benchmark.cc zhttp)
endif()
