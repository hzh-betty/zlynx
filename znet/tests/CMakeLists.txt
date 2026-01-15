cmake_minimum_required(VERSION 3.18)

project(znet_tests LANGUAGES CXX)

# 查找GTest
find_package(GTest REQUIRED)

# 启用测试
enable_testing()

# 收集测试源文件
file(GLOB TEST_UNIT_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/unit/*.cc)
file(GLOB TEST_INTEGRATION_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/integration/*.cc)

# ========================= 单元测试 =========================
if(TEST_UNIT_SRCS)
    foreach(test_source ${TEST_UNIT_SRCS})
        # 获取文件名（不含路径和扩展名）
        get_filename_component(test_name ${test_source} NAME_WE)
        
        # 创建可执行文件
        add_executable(${test_name} ${test_source})
        
        # 链接库
        target_link_libraries(${test_name} PRIVATE
                znet_shared
                zcoroutine_shared
                zlog_shared
                GTest::gtest
                pthread
        )
        
        # 添加为CTest测试
        add_test(NAME ${test_name} COMMAND ${test_name})
        
        message(STATUS "Configured unit test: ${test_name}")
    endforeach()
endif()

# ========================= 集成测试 =========================
if(TEST_INTEGRATION_SRCS)
    foreach(test_source ${TEST_INTEGRATION_SRCS})
        # 获取文件名（不含路径和扩展名）
        get_filename_component(test_name ${test_source} NAME_WE)
        
        # 创建可执行文件
        add_executable(${test_name} ${test_source})
        
        # 链接库
        target_link_libraries(${test_name} PRIVATE
                znet_shared
                zcoroutine_shared
                zlog_shared
                GTest::gtest
                pthread
        )
        
        # 添加为CTest测试
        add_test(NAME ${test_name} COMMAND ${test_name})
        
        message(STATUS "Configured integration test: ${test_name}")
    endforeach()
endif()

# ========================= 性能测试 =========================
# 性能测试基准程序
file(GLOB BENCHMARK_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/benchmark/*_bench.cc)

foreach(bench_source ${BENCHMARK_SRCS})
    get_filename_component(bench_name ${bench_source} NAME_WE)
    
    # 添加前缀避免与zcoroutine的benchmark冲突
    set(bench_target_name "znet_${bench_name}")
    
    # 创建可执行文件
    add_executable(${bench_target_name} ${bench_source})
    
    # 链接库
    target_link_libraries(${bench_target_name} PRIVATE
            znet_shared
            zcoroutine_shared
            zlog_shared
            pthread
    )
    # 使用 -O2 优化级别和调试符号（用于perf）
    target_compile_options(${bench_target_name} PRIVATE -O2 -g)
    message(STATUS "Configured benchmark: ${bench_target_name}")
endforeach()

message(STATUS "==============================")
message(STATUS "znet tests configuration")
message(STATUS "------------------------------")
message(STATUS "Unit tests        : ${TEST_UNIT_SRCS}")
message(STATUS "Integration tests : ${TEST_INTEGRATION_SRCS}")
message(STATUS "Benchmark tests   : ${BENCHMARK_SRCS}")
message(STATUS "==============================")
