cmake_minimum_required(VERSION 3.18)

# 测试配置
find_package(GTest QUIET)

if(NOT GTest_FOUND)
    message(STATUS "GTest not found, zmalloc tests disabled")
    return()
endif()

enable_testing()

find_package(Threads REQUIRED)

# zmalloc 头文件目录
set(ZMALLOC_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)

# 单元测试
set(UNIT_TESTS
    size_class_test
    free_list_test
    object_pool_test
    transfer_cache_test
    zmalloc_test
    page_map_test
    page_cache_test
    central_cache_test
    thread_cache_test
)

foreach(TEST_NAME ${UNIT_TESTS})
    add_executable(zmalloc_${TEST_NAME} unit/${TEST_NAME}.cc)
    target_link_libraries(zmalloc_${TEST_NAME}
        PRIVATE
            zmalloc
            GTest::gtest
    )
    target_include_directories(zmalloc_${TEST_NAME} PRIVATE ${ZMALLOC_INCLUDE_DIR})
    add_test(NAME zmalloc_${TEST_NAME} COMMAND zmalloc_${TEST_NAME})
endforeach()

# 集成测试
set(INTEGRATION_TESTS
    concurrent_alloc_test
)

foreach(TEST_NAME ${INTEGRATION_TESTS})
    add_executable(zmalloc_${TEST_NAME} integration/${TEST_NAME}.cc)
    target_link_libraries(zmalloc_${TEST_NAME}
        PRIVATE
            zmalloc
            GTest::gtest
    )
    target_include_directories(zmalloc_${TEST_NAME} PRIVATE ${ZMALLOC_INCLUDE_DIR})
    add_test(NAME zmalloc_${TEST_NAME} COMMAND zmalloc_${TEST_NAME})
endforeach()

# 基准测试
add_executable(zmalloc_benchmark benchmark/benchmark.cc)
target_link_libraries(zmalloc_benchmark
    PRIVATE
        zmalloc
        Threads::Threads
)
target_include_directories(zmalloc_benchmark PRIVATE ${ZMALLOC_INCLUDE_DIR})

# perf 性能分析驱动
add_executable(zmalloc_performance benchmark/performance.cc)
target_link_libraries(zmalloc_performance
    PRIVATE
        zmalloc
        Threads::Threads
)
target_include_directories(zmalloc_performance PRIVATE ${ZMALLOC_INCLUDE_DIR})
target_compile_options(zmalloc_performance PRIVATE -g -O2)
