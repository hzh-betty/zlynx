cmake_minimum_required(VERSION 3.18)

find_package(GTest REQUIRED)
find_package(Threads REQUIRED)

# znet 的网络测试对 socket/thread 资源敏感；仅 unit/integration 注册进 CTest。
if(BUILD_TESTING)
    file(GLOB ZNET_UNIT_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/unit/*.cc")
    file(GLOB ZNET_INTEGRATION_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/integration/*.cc")

    foreach(test_src ${ZNET_UNIT_TESTS})
        get_filename_component(test_name ${test_src} NAME_WE)
        zlynx_add_gtest(${test_name} ${test_src} znet unit znet)
    endforeach()

    foreach(test_src ${ZNET_INTEGRATION_TESTS})
        get_filename_component(test_name ${test_src} NAME_WE)
        zlynx_add_gtest(${test_name} ${test_src} znet integration znet)
    endforeach()

    foreach(test_name buffer_test server_test tcp_server_test)
        if(TARGET ${test_name})
            # 这些测试在高并发 CTest 下更容易受运行时/socket 资源影响，保持串行。
            set(serial_test_kind unit)
            if(test_name STREQUAL "tcp_server_test")
                set(serial_test_kind integration)
            endif()
            set_tests_properties(znet.${serial_test_kind}.${test_name} PROPERTIES
                RUN_SERIAL TRUE
                RESOURCE_LOCK zco_runtime
            )
        endif()
    endforeach()

    zlynx_add_module_test_targets(znet)
endif()

if(ZLYNX_BUILD_PERF_TESTS)
    # wrk benchmark 只生成二进制，不注册 CTest，避免默认验证跑压测。
    zlynx_add_perf_target(znet_wrk_benchmark benchmark/znet_wrk_benchmark.cc znet)
endif()
