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
        span_list_test
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

    set(ZMALLOC_OVERRIDE_TEST_TARGETS
        zmalloc_override_whitebox_test
        zmalloc_allocator_override_test
    )
    if(BUILD_SHARED_LIBS)
        # 实际共享对象验证 ELF 替换及 TLS，复用公开接口测试。
        add_library(zmalloc_override_shared_test SHARED ../src/override.cc)
        target_link_libraries(zmalloc_override_shared_test PUBLIC zmalloc PRIVATE ${CMAKE_DL_LIBS})
        zlynx_apply_common_options(zmalloc_override_shared_test)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(zmalloc_override_shared_test PRIVATE -ftls-model=initial-exec)
        endif()
        zlynx_add_gtest(zmalloc_allocator_override_shared_test unit/allocator_override_test.cc zmalloc unit zmalloc_override_shared_test)
        list(APPEND ZMALLOC_OVERRIDE_TEST_TARGETS zmalloc_allocator_override_shared_test)
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        foreach(test_target ${ZMALLOC_OVERRIDE_TEST_TARGETS})
            # 必须实际调用替换入口，不能由编译器折叠边界分配请求。
            target_compile_options(${test_target} PRIVATE -fno-builtin)
        endforeach()
    endif()

    set(ZMALLOC_INTEGRATION_TESTS
        concurrent_alloc_test
    )

    foreach(test_name ${ZMALLOC_INTEGRATION_TESTS})
        zlynx_add_gtest(zmalloc_${test_name} integration/${test_name}.cc zmalloc integration zmalloc)
    endforeach()

    # 冷启动复现独立运行，确保首次小/大对象分配发生在两个并发线程中。
    add_executable(zmalloc_cold_start_test integration/cold_start_test.cc)
    target_link_libraries(zmalloc_cold_start_test PRIVATE zmalloc zlynx_coverage_options)
    add_test(NAME zmalloc.integration.zmalloc_cold_start_test COMMAND zmalloc_cold_start_test)
    set_tests_properties(zmalloc.integration.zmalloc_cold_start_test PROPERTIES
        LABELS "integration;total"
        TIMEOUT 30
    )

    zlynx_add_module_test_targets(zmalloc)
endif()

if(ZLYNX_BUILD_PERF_TESTS)
    # 性能目标使用 RelWithDebInfo preset 和 zlynx_perf_options，便于 perf 采样。
    zlynx_add_perf_target(zmalloc_thread_cache_benchmark benchmark/thread_cache_benchmark.cc zmalloc)
    zlynx_add_perf_target(zmalloc_benchmark benchmark/zmalloc_benchmark.cc zmalloc)
    zlynx_add_perf_target(zmalloc_performance benchmark/zmalloc_performance.cc zmalloc)
endif()
