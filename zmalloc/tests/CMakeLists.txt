cmake_minimum_required(VERSION 3.18)

find_package(GTest QUIET)
find_package(Threads REQUIRED)

if(NOT GTest_FOUND)
    message(STATUS "zmalloc tests disabled: GTest was not found")
    return()
endif()

# unit/integration 是常规验证目标，会注册进 CTest；benchmark/performance 单独构建。
if(BUILD_TESTING)
    # allocator 相关测试需要明确列出，避免 override 测试误链接到普通 zmalloc 目标。
    set(ZMALLOC_UNIT_TESTS
        size_class_test
        free_list_test
        object_pool_test
        transfer_cache_test
        zmalloc_test
        page_map_test
        page_cache_test
        central_cache_test
        thread_cache_test
        system_alloc_test
        override_whitebox_test
    )

    foreach(test_name ${ZMALLOC_UNIT_TESTS})
        zlynx_add_gtest(zmalloc_${test_name} unit/${test_name}.cc zmalloc unit zmalloc)
    endforeach()

    zlynx_add_gtest(zmalloc_allocator_override_test unit/allocator_override_test.cc zmalloc unit zmalloc_override)

    set(ZMALLOC_INTEGRATION_TESTS
        concurrent_alloc_test
    )

    foreach(test_name ${ZMALLOC_INTEGRATION_TESTS})
        zlynx_add_gtest(zmalloc_${test_name} integration/${test_name}.cc zmalloc integration zmalloc)
    endforeach()
endif()

if(ZLYNX_BUILD_PERF_TESTS)
    # 性能目标使用 RelWithDebInfo preset 和 zlynx_perf_options，便于 perf 采样。
    zlynx_add_perf_target(zmalloc_benchmark benchmark/zmalloc_benchmark.cc zmalloc)
    zlynx_add_perf_target(zmalloc_performance benchmark/zmalloc_performance.cc zmalloc)
endif()
