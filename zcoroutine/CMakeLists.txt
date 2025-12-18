cmake_minimum_required(VERSION 3.10)
project(zcoroutine VERSION 1.0.0 LANGUAGES CXX)

# 设置C++标准为C++14
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 设置编译选项
if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Wall -Wextra -Werror -Wno-unused-parameter)
endif()

# 头文件目录
include_directories(
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/include/runtime
    ${PROJECT_SOURCE_DIR}/include/scheduling
    ${PROJECT_SOURCE_DIR}/include/io
    ${PROJECT_SOURCE_DIR}/include/timer
    ${PROJECT_SOURCE_DIR}/include/hook
    ${PROJECT_SOURCE_DIR}/include/sync
    ${PROJECT_SOURCE_DIR}/include/util
)

# 依赖zlog头文件
include_directories(${PROJECT_SOURCE_DIR}/../zlog/include)

# 收集源文件
# 工具类源文件
file(GLOB UTIL_SRCS 
    ${PROJECT_SOURCE_DIR}/src/util/*.cc
)

# 同步原语源文件
file(GLOB SYNC_SRCS 
    ${PROJECT_SOURCE_DIR}/src/sync/*.cc
)

# 运行时层源文件
file(GLOB RUNTIME_SRCS 
    ${PROJECT_SOURCE_DIR}/src/runtime/*.cc
)

# 调度层源文件
file(GLOB SCHEDULING_SRCS 
    ${PROJECT_SOURCE_DIR}/src/scheduling/*.cc
)

# IO层源文件
file(GLOB IO_SRCS 
    ${PROJECT_SOURCE_DIR}/src/io/*.cc
)

# 定时器层源文件
file(GLOB TIMER_SRCS 
    ${PROJECT_SOURCE_DIR}/src/timer/*.cc
)

# Hook层源文件
file(GLOB HOOK_SRCS 
    ${PROJECT_SOURCE_DIR}/src/hook/*.cc
)

# 合并所有源文件
set(ZCOROUTINE_SRCS
    ${UTIL_SRCS}
    ${SYNC_SRCS}
    ${RUNTIME_SRCS}
    ${SCHEDULING_SRCS}
    ${IO_SRCS}
    ${TIMER_SRCS}
    ${HOOK_SRCS}
)

# 创建静态库
add_library(zcoroutine_static STATIC ${ZCOROUTINE_SRCS})
set_target_properties(zcoroutine_static PROPERTIES OUTPUT_NAME zcoroutine)

# 链接依赖库
target_link_libraries(zcoroutine_static 
    pthread      # 线程库
    dl           # 动态链接库（用于dlsym）
)

# 如果构建类型是Debug，添加调试符号
if(CMAKE_BUILD_TYPE MATCHES Debug)
    target_compile_options(zcoroutine_static PRIVATE -g -O0)
else()
    target_compile_options(zcoroutine_static PRIVATE -O2)
endif()

# 安装配置（可选）
install(TARGETS zcoroutine_static
    ARCHIVE DESTINATION lib
)

install(DIRECTORY include/
    DESTINATION include/zcoroutine
    FILES_MATCHING PATTERN "*.h"
)

# 输出信息
message(STATUS "zcoroutine library configuration:")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")

# ==============================================================================
# 测试支持
# ==============================================================================
option(BUILD_TESTS "Build unit tests" ON)
option(BUILD_BENCHMARKS "Build benchmark tests" ON)

if(BUILD_TESTS)
    enable_testing()
    
    # 查找 GTest
    find_package(GTest REQUIRED)
    include_directories(${GTEST_INCLUDE_DIRS})
    
    # 单元测试
    file(GLOB_RECURSE UNIT_TEST_SRCS ${PROJECT_SOURCE_DIR}/tests/unit/*.cc)
    if(UNIT_TEST_SRCS)
        add_executable(zcoroutine_unit_tests ${UNIT_TEST_SRCS})
        target_link_libraries(zcoroutine_unit_tests
            zcoroutine_static
            ${GTEST_BOTH_LIBRARIES}
            pthread
            dl
        )
        add_test(NAME UnitTests COMMAND zcoroutine_unit_tests)
    endif()
    
    # 集成测试
    file(GLOB_RECURSE INTEGRATION_TEST_SRCS ${PROJECT_SOURCE_DIR}/tests/integration/*.cc)
    if(INTEGRATION_TEST_SRCS)
        add_executable(zcoroutine_integration_tests ${INTEGRATION_TEST_SRCS})
        target_link_libraries(zcoroutine_integration_tests
            zcoroutine_static
            ${GTEST_BOTH_LIBRARIES}
            pthread
            dl
        )
        add_test(NAME IntegrationTests COMMAND zcoroutine_integration_tests)
    endif()
endif()

if(BUILD_BENCHMARKS)
    # 查找 Google Benchmark
    find_package(benchmark QUIET)
    if(benchmark_FOUND)
        file(GLOB_RECURSE BENCHMARK_SRCS ${PROJECT_SOURCE_DIR}/tests/benchmark/*.cc)
        if(BENCHMARK_SRCS)
            add_executable(zcoroutine_benchmarks ${BENCHMARK_SRCS})
            target_link_libraries(zcoroutine_benchmarks
                zcoroutine_static
                benchmark::benchmark
                pthread
                dl
            )
        endif()
    else()
        message(WARNING "Google Benchmark not found, benchmarks will not be built")
    endif()
endif()

# ==============================================================================
# 示例程序
# ==============================================================================
file(GLOB_RECURSE EXAMPLE_SRCS ${PROJECT_SOURCE_DIR}/examples/*.cc)
foreach(EXAMPLE_SRC ${EXAMPLE_SRCS})
    get_filename_component(EXAMPLE_NAME ${EXAMPLE_SRC} NAME_WE)
    add_executable(${EXAMPLE_NAME} ${EXAMPLE_SRC})
    target_link_libraries(${EXAMPLE_NAME}
        zcoroutine_static
        pthread
        dl
    )
endforeach()
