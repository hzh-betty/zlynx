# zlynx 全项目公共构建选项。
#
# 这个文件只定义“可复用的 target 级能力”，不调用 include_directories()、
# link_libraries() 或 add_compile_options() 这类目录/全局作用域命令。
# 这样每个库目标都能清晰声明自己的编译特性、告警、覆盖率和性能测试参数，
# install/export 后也不会把仓库内部 helper target 泄漏给使用方。

include_guard(GLOBAL)

include(GNUInstallDirs)

# 覆盖率只对支持 --coverage 的 GNU/Clang 生效，并且按 target 注入。
option(ZLYNX_ENABLE_COVERAGE "Enable coverage instrumentation for supported compilers" OFF)

# allocator override 是运行时选择，不作为各模块安装包的 PUBLIC 依赖导出。
option(ZLYNX_USE_ZMALLOC_OVERRIDE "Link zmalloc_override into runtime modules" ON)

# perf 目标可以被构建和手动运行，但不会注册进 CTest。
option(ZLYNX_BUILD_PERF_TESTS "Build benchmark/performance binaries without registering them in CTest" OFF)

set(ZLYNX_PERF_COMPILE_OPTIONS
    "-fno-omit-frame-pointer"
    CACHE STRING "Semicolon-separated compile options for performance test targets"
)

# 告警 helper 只在源码树内部使用。可安装库目标不会 PUBLIC 链接它，避免
# install(EXPORT) 要求下游也存在 zlynx_project_warnings。
add_library(zlynx_project_warnings INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(zlynx_project_warnings INTERFACE -Wall -Wextra -Wpedantic)
endif()

add_library(zlynx_coverage_options INTERFACE)
if(ZLYNX_ENABLE_COVERAGE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(zlynx_coverage_options INTERFACE -O0 -g --coverage)
        target_link_options(zlynx_coverage_options INTERFACE --coverage)
    else()
        message(WARNING "ZLYNX_ENABLE_COVERAGE is ON, but compiler ${CMAKE_CXX_COMPILER_ID} is not supported")
    endif()
endif()

add_library(zlynx_perf_options INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(zlynx_perf_options INTERFACE ${ZLYNX_PERF_COMPILE_OPTIONS})
endif()

function(zlynx_apply_common_options target_name)
    # PUBLIC 编译特性会被导出为目标用法需求：消费者链接库时也使用 C++14。
    target_compile_features(${target_name} PUBLIC cxx_std_14)

    # warning/coverage 是项目内部构建策略，使用 PRIVATE，避免污染外部消费者。
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    if(ZLYNX_ENABLE_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target_name} PRIVATE -O0 -g --coverage)
        target_link_options(${target_name} PRIVATE --coverage)
    endif()
endfunction()

function(zlynx_apply_perf_options target_name)
    # 性能目标使用当前构建类型的优化策略。perf preset 指定 RelWithDebInfo，
    # 这里额外补 frame pointer，方便 perf、火焰图等采样工具还原调用栈。
    target_link_libraries(${target_name}
        PRIVATE
            zlynx_perf_options
    )
endfunction()
