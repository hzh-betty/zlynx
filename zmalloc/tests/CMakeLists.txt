cmake_minimum_required(VERSION 3.18)

project(zmalloc_tests LANGUAGES CXX)

# 查找GTest
find_package(GTest REQUIRED)

# 启用测试
enable_testing()

# 收集测试源文件
file(GLOB TEST_UNIT_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/unit/*.cc)
file(GLOB TEST_INTEGRATION_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/integration/*.cc)

# 单元测试
if(TEST_UNIT_SRCS)
    foreach(test_source ${TEST_UNIT_SRCS})
        get_filename_component(test_name ${test_source} NAME_WE)

        add_executable(zmalloc_${test_name} ${test_source})

        target_link_libraries(zmalloc_${test_name} PRIVATE
            zmalloc_static
            GTest::gtest
            GTest::gtest_main
            pthread
        )

        add_test(NAME zmalloc_${test_name} COMMAND zmalloc_${test_name})

        message(STATUS "Configured zmalloc unit test: zmalloc_${test_name}")
    endforeach()
endif()

# 集成测试
if(TEST_INTEGRATION_SRCS)
    foreach(test_source ${TEST_INTEGRATION_SRCS})
        get_filename_component(test_name ${test_source} NAME_WE)

        add_executable(zmalloc_${test_name} ${test_source})

        target_link_libraries(zmalloc_${test_name} PRIVATE
            zmalloc_static
            GTest::gtest
            GTest::gtest_main
            pthread
        )

        add_test(NAME zmalloc_${test_name} COMMAND zmalloc_${test_name})

        message(STATUS "Configured zmalloc integration test: zmalloc_${test_name}")
    endforeach()
endif()

message(STATUS "==============================")
message(STATUS "zmalloc tests configuration")
message(STATUS "------------------------------")
message(STATUS "Unit tests        : ${TEST_UNIT_SRCS}")
message(STATUS "Integration tests : ${TEST_INTEGRATION_SRCS}")
message(STATUS "==============================")
