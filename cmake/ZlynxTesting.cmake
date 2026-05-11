# 测试目标公共封装。
#
# 约定：
# - unit/integration 使用 zlynx_add_gtest()，自动注册到 CTest；
# - stress 使用 zlynx_add_standalone_test()，只生成可执行文件，不进入 CTest；
# - perf/benchmark 使用 zlynx_add_perf_target()，只生成可执行文件，并附加性能参数；
# - labels 同时包含模块名和测试分类，便于 `ctest -L zco -L unit` 过滤。

include_guard(GLOBAL)

include(CTest)

function(zlynx_add_gtest target_name source_file module_name test_kind library_target)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name}
        PRIVATE
            ${library_target}
            GTest::gtest
            GTest::gmock
            Threads::Threads
            zlynx_coverage_options
    )
    # CTest 名称带模块和分类前缀，避免不同模块出现同名测试时冲突。
    add_test(NAME ${module_name}.${test_kind}.${target_name} COMMAND ${target_name})
    set_tests_properties(${module_name}.${test_kind}.${target_name}
        PROPERTIES
            LABELS "${module_name};${test_kind}"
            TIMEOUT 90
    )
endfunction()

function(zlynx_add_standalone_test target_name source_file library_target)
    # stress 测试可能耗时长、消耗系统资源或依赖特殊环境，因此不调用 add_test()。
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name}
        PRIVATE
            ${library_target}
            GTest::gtest
            GTest::gmock
            Threads::Threads
            zlynx_coverage_options
    )
endfunction()

function(zlynx_add_perf_target target_name source_file library_target)
    # perf 目标用于手动基准测试/采样分析，不注册 CTest，也不注入 coverage。
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name}
        PRIVATE
            ${library_target}
            Threads::Threads
    )
    zlynx_apply_perf_options(${target_name})
endfunction()
