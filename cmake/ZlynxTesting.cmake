# 测试目标公共封装。
#
# 约定：
# - unit/integration 使用 zlynx_add_gtest()，自动注册到 CTest；
# - perf/benchmark 使用 zlynx_add_perf_target()，只生成可执行文件，并附加性能参数；
# - labels 包含测试分类和 total，让模块级 CTest 汇总输出 unit/integration/total。

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
            LABELS "${test_kind};total"
            TIMEOUT 90
    )
endfunction()

function(zlynx_add_module_test_targets module_name)
    add_custom_target(${module_name}_test
        COMMAND ${CMAKE_CTEST_COMMAND}
                -R "^${module_name}\\."
                --output-on-failure
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Run ${module_name} tests with unit/integration/total summary"
    )

    add_custom_target(${module_name}_test_unit
        COMMAND ${CMAKE_CTEST_COMMAND}
                -R "^${module_name}\\.unit\\."
                --output-on-failure
                --no-label-summary
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Run ${module_name} unit tests"
    )

    add_custom_target(${module_name}_test_integration
        COMMAND ${CMAKE_CTEST_COMMAND}
                -R "^${module_name}\\.integration\\."
                --output-on-failure
                --no-label-summary
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Run ${module_name} integration tests"
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
