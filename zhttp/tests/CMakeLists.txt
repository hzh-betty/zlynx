if(BUILD_TESTING)
    if(NOT TARGET GTest::gtest_main)
        find_package(GTest CONFIG REQUIRED)
    endif()

    set(test_sources
        integration/http_server_test.cc
        integration/websocket_server_test.cc
        unit/auth_middleware_test.cc
        unit/compression_middleware_test.cc
        unit/cors_middleware_test.cc
        unit/daemon_test.cc
        unit/error_middleware_test.cc
        unit/http_application_test.cc
        unit/http_common_test.cc
        unit/http_parser_test.cc
        unit/http_request_test.cc
        unit/http_response_test.cc
        unit/http_response_writer_test.cc
        unit/http_server_builder_test.cc
        unit/http_server_context_test.cc
        unit/logging_test.cc
        unit/middleware_test.cc
        unit/multipart_test.cc
        unit/range_parse_test.cc
        unit/rate_limiter_test.cc
        unit/regex_prefix_bucket_test.cc
        unit/request_body_middleware_test.cc
        unit/request_components_test.cc
        unit/router_detailed_test.cc
        unit/router_test.cc
        unit/security_middleware_test.cc
        unit/server_config_test.cc
        unit/session_test.cc
        unit/socket_reader_test.cc
        unit/static_file_opt_test.cc
        unit/static_path_test.cc
        unit/static_resource_store_test.cc
        unit/timeout_middleware_test.cc
        unit/websocket_frame_test.cc
        unit/websocket_handshake_test.cc
    )
    foreach(source IN LISTS test_sources)
        get_filename_component(name ${source} NAME_WE)
        add_executable(zhttp_${name} ${source})
        target_compile_features(zhttp_${name} PRIVATE cxx_std_17)
        set_target_properties(zhttp_${name} PROPERTIES CXX_EXTENSIONS OFF)
        target_link_libraries(zhttp_${name} PRIVATE zhttp::zhttp GTest::gtest_main Threads::Threads)
        target_include_directories(zhttp_${name} PRIVATE ../src)
        add_test(NAME zhttp_${name} COMMAND zhttp_${name})
        set_tests_properties(zhttp_${name} PROPERTIES TIMEOUT 90)
    endforeach()

    foreach(clock_test session_test timeout_middleware_test)
        target_sources(zhttp_${clock_test} PRIVATE support/manual_clock.cc)
        target_link_options(zhttp_${clock_test} PRIVATE
            LINKER:--wrap=_ZNSt6chrono3_V212steady_clock3nowEv)
    endforeach()
    if(BUILD_SHARED_LIBS)
        # 仅将依赖时钟的实现编译到使用链接包装的测试程序中。
        target_sources(zhttp_session_test PRIVATE ../src/session/session.cc)
        target_sources(zhttp_timeout_middleware_test PRIVATE ../src/middleware/timeout_middleware.cc)
    endif()
    pkg_check_modules(BrotliDecoder REQUIRED IMPORTED_TARGET libbrotlidec)
    target_link_libraries(zhttp_compression_middleware_test PRIVATE ZLIB::ZLIB PkgConfig::BrotliDecoder)
    target_link_libraries(zhttp_logging_test PRIVATE zlog::zlog)
    target_link_libraries(zhttp_http_server_test PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

if(BUILD_BENCHMARKS)
    add_executable(zhttp_benchmark benchmark/zhttp_benchmark.cc)
    target_link_libraries(zhttp_benchmark PRIVATE zhttp::zhttp Threads::Threads)
    add_executable(zhttp_core_benchmark benchmark/zhttp_core_benchmark.cc)
    target_link_libraries(zhttp_core_benchmark PRIVATE zhttp::zhttp Threads::Threads)
    target_include_directories(zhttp_core_benchmark PRIVATE ../src)
endif()
