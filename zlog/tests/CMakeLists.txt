cmake_minimum_required(VERSION 3.18)

find_package(GTest REQUIRED)

find_package(benchmark QUIET)

find_package(spdlog QUIET)

find_package(glog QUIET)

function(zlog_test_enable_coverage target_name)
    if(ENABLE_COVERAGE)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(${target_name} PRIVATE -O0 -g --coverage)
            target_link_options(${target_name} PRIVATE --coverage)
        endif()
    endif()
endfunction()

# 收集单元测试源文件
file(GLOB UNIT_TEST_SOURCES "unit/*.cc")
message(STATUS "Project source dir: ${PROJECT_SOURCE_DIR}")
foreach(TEST_SRC ${UNIT_TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)
    add_executable(${TEST_NAME} ${TEST_SRC})
    target_link_libraries(${TEST_NAME} PRIVATE zlog_static GTest::gtest GTest::gmock Threads::Threads)
    target_include_directories(${TEST_NAME} PRIVATE ${PROJECT_SOURCE_DIR}/include)
    zlog_test_enable_coverage(${TEST_NAME})
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endforeach()

# 收集集成测试源文件
file(GLOB INTEGRATION_TEST_SOURCES "integration/*.cc")
foreach(TEST_SRC ${INTEGRATION_TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)
    add_executable(${TEST_NAME} ${TEST_SRC})
    target_link_libraries(${TEST_NAME} PRIVATE zlog_static GTest::gtest GTest::gmock Threads::Threads)
    target_include_directories(${TEST_NAME} PRIVATE ${PROJECT_SOURCE_DIR}/include)
    zlog_test_enable_coverage(${TEST_NAME})
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endforeach()

# 基准测试
if(spdlog_FOUND AND glog_FOUND)
    add_executable(zlog_benchmark benchmark/bench_compare.cc)
    target_link_libraries(zlog_benchmark PRIVATE 
        zlog_static 
        spdlog::spdlog
        glog::glog
        Threads::Threads
    )
    target_include_directories(zlog_benchmark PRIVATE ${PROJECT_SOURCE_DIR}/include)
    zlog_test_enable_coverage(zlog_benchmark)
else()
    message(STATUS "Skipping zlog_benchmark (requires spdlog, glog)")
endif()

# 性能测试
add_executable(zlog_perf_bench benchmark/perf_bench.cc)
target_link_libraries(zlog_perf_bench PRIVATE 
    zlog_static 
    Threads::Threads
)
target_include_directories(zlog_perf_bench PRIVATE ${PROJECT_SOURCE_DIR}/include)
zlog_test_enable_coverage(zlog_perf_bench)
