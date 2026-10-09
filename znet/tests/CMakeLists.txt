cmake_minimum_required(VERSION 3.18)

# znet 的网络测试对 socket/thread 资源敏感；仅 unit/integration 注册进 CTest。
if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    find_package(Threads REQUIRED)

    file(GLOB ZNET_UNIT_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/unit/*.cc")
    file(GLOB ZNET_INTEGRATION_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/integration/*.cc")

    foreach(test_src ${ZNET_UNIT_TESTS})
        get_filename_component(test_name ${test_src} NAME_WE)
        zlynx_add_gtest(${test_name} ${test_src} znet unit znet)
        target_link_libraries(${test_name} PRIVATE GTest::gtest_main)
    endforeach()

    foreach(test_src ${ZNET_INTEGRATION_TESTS})
        get_filename_component(test_name ${test_src} NAME_WE)
        zlynx_add_gtest(${test_name} ${test_src} znet integration znet)
        target_link_libraries(${test_name} PRIVATE GTest::gtest_main)
    endforeach()

    # Only the TLS client integration test directly uses OpenSSL.
    target_link_libraries(tls_stream_test PRIVATE OpenSSL::SSL OpenSSL::Crypto)

    zlynx_add_module_test_targets(znet)
endif()

if(ZLYNX_BUILD_PERF_TESTS)
    # wrk benchmark 只生成二进制，不注册 CTest，避免默认验证跑压测。
    zlynx_add_perf_target(znet_wrk_benchmark benchmark/znet_wrk_benchmark.cc znet)
endif()
