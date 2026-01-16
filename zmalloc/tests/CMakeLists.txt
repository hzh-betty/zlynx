cmake_minimum_required(VERSION 3.18)

# 测试配置
find_package(GTest QUIET)

if(NOT GTest_FOUND)
    message(STATUS "GTest not found, zmalloc tests disabled")
    return()
endif()

enable_testing()

# zmalloc 头文件目录
set(ZMALLOC_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)

# 单元测试
set(UNIT_TESTS
    size_class_test
    free_list_test
    object_pool_test
    zmalloc_test
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
)
target_include_directories(zmalloc_benchmark PRIVATE ${ZMALLOC_INCLUDE_DIR})
