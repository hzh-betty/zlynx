option(ENABLE_COVERAGE "Enable code coverage generation" OFF)

function(zcoroutine_apply_coverage target_name)
    if(NOT TARGET ${target_name})
        return()
    endif()

    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        return()
    endif()

    target_compile_options(${target_name} PRIVATE
            -fprofile-arcs
            -ftest-coverage
            -fprofile-update=atomic
    )

    get_target_property(target_type ${target_name} TYPE)
    if(NOT target_type STREQUAL "STATIC_LIBRARY")
        target_link_options(${target_name} PRIVATE
                -fprofile-arcs
                -ftest-coverage
                -fprofile-update=atomic
        )
    endif()
endfunction()

find_package(GTest REQUIRED)

if(ENABLE_COVERAGE)
    message(STATUS "Enabling code coverage")
    zcoroutine_apply_coverage(zcoroutine_shared)
    zcoroutine_apply_coverage(zcoroutine_static)
endif()

# 收集测试源文件
file(GLOB TEST_UNIT_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/unit/*.cc)
file(GLOB TEST_INTEGRATION_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/integration/*.cc)

# 为每个单元测试文件创建独立的测试目标
if(TEST_UNIT_SRCS)
    foreach(test_source ${TEST_UNIT_SRCS})
        # 获取文件名（不含路径和扩展名）
        get_filename_component(test_name ${test_source} NAME_WE)
        # 创建可执行文件
        add_executable(${test_name} ${test_source})
        # 链接库
        target_link_libraries(${test_name} PRIVATE
                zcoroutine_shared
                GTest::gtest
                pthread
        )
        if(ENABLE_COVERAGE)
            zcoroutine_apply_coverage(${test_name})
        endif()
        # 添加为CTest测试
        add_test(NAME ${test_name} COMMAND ${test_name})
        message(STATUS "Configured unit test: ${test_name}")
    endforeach()
endif()

# 为每个集成测试文件创建独立的测试目标
if(TEST_INTEGRATION_SRCS)
    foreach(test_source ${TEST_INTEGRATION_SRCS})
        # 获取文件名（不含路径和扩展名）
        get_filename_component(test_name ${test_source} NAME_WE)
        # 创建可执行文件
        add_executable(${test_name} ${test_source})
        # 链接库
        target_link_libraries(${test_name} PRIVATE
                zcoroutine_shared
                GTest::gtest
                pthread
        )
        if(ENABLE_COVERAGE)
            zcoroutine_apply_coverage(${test_name})
        endif()
        # 添加为CTest测试
        add_test(NAME ${test_name} COMMAND ${test_name})
        message(STATUS "Configured integration test: ${test_name}")
    endforeach()
endif()

# 新的性能测试基准程序
file(GLOB BENCHMARK_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/benchmark/*_bench.cc)
foreach(bench_source ${BENCHMARK_SRCS})
    get_filename_component(bench_name ${bench_source} NAME_WE)
    add_executable(${bench_name} ${bench_source})
    target_link_libraries(${bench_name} PRIVATE
            zcoroutine_shared
            pthread
    )
    # 使用 -O2 优化级别和调试符号（用于perf）
    target_compile_options(${bench_name} PRIVATE -O2 -g)
    message(STATUS "Configured benchmark: ${bench_name}")
endforeach()