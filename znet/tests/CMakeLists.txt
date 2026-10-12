if(BUILD_TESTING)
    if(NOT TARGET GTest::gtest_main)
        find_package(GTest CONFIG REQUIRED)
    endif()

    set(test_sources
        integration/connection_io_test.cc
        integration/socket_test.cc
        integration/tcp_server_test.cc
        integration/tls_stream_test.cc
        unit/byte_buffer_test.cc
        unit/connection_test.cc
        unit/endpoint_test.cc
        unit/error_test.cc
    )
    foreach(source IN LISTS test_sources)
        get_filename_component(name ${source} NAME_WE)
        add_executable(znet_${name} ${source})
        target_compile_features(znet_${name} PRIVATE cxx_std_17)
        set_target_properties(znet_${name} PROPERTIES CXX_EXTENSIONS OFF)
        target_link_libraries(znet_${name} PRIVATE znet::znet GTest::gtest_main Threads::Threads)
        add_test(NAME znet_${name} COMMAND znet_${name})
        set_tests_properties(znet_${name} PROPERTIES TIMEOUT 90)
    endforeach()

    find_program(ZNET_TEST_OPENSSL NAMES openssl REQUIRED)
    target_compile_definitions(znet_tls_stream_test PRIVATE ZNET_TEST_OPENSSL="${ZNET_TEST_OPENSSL}")
    target_link_libraries(znet_tls_stream_test PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

if(BUILD_BENCHMARKS)
    add_executable(znet_wrk_benchmark benchmark/znet_wrk_benchmark.cc)
    target_link_libraries(znet_wrk_benchmark PRIVATE znet::znet Threads::Threads)
endif()
