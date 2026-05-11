cmake_minimum_required(VERSION 3.18)

find_package(GTest REQUIRED)
find_package(Threads REQUIRED)

set(zco_test_runtime zco)
if(TARGET zco_stdalloc)
    # 测试优先使用不带 allocator override 的运行时，避免 allocator 行为影响协程测试。
    set(zco_test_runtime zco_stdalloc)
endif()

function(add_zco_gtest target_name src_file labels timeout_seconds)
    add_executable(${target_name} ${src_file})
    target_link_libraries(${target_name} PRIVATE ${zco_test_runtime} GTest::gtest Threads::Threads zlynx_coverage_options)
    target_include_directories(${target_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME zco.${labels}.${target_name} COMMAND ${target_name})
    set_tests_properties(zco.${labels}.${target_name} PROPERTIES LABELS "${labels};total" TIMEOUT ${timeout_seconds})
endfunction()

file(GLOB ZCO_UNIT_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/unit/*.cc")
file(GLOB ZCO_INTEGRATION_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/integration/*.cc")
set(ZCO_PERF_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/benchmark/zco_performance.cc")

# CTest 只纳入 unit/integration；perf 通过独立开关生成可执行文件。
if(BUILD_TESTING)
    foreach(test_src ${ZCO_UNIT_TESTS})
        get_filename_component(test_name ${test_src} NAME_WE)
        add_zco_gtest(${test_name} ${test_src} unit 20)
    endforeach()

    foreach(test_src ${ZCO_INTEGRATION_TESTS})
        get_filename_component(test_name ${test_src} NAME_WE)
        add_zco_gtest(${test_name} ${test_src} integration 45)
    endforeach()

    if(TARGET hook_integration_test)
        # hook_integration_test 依赖运行时和 socket 资源，串行执行可降低并发压力。
        set_tests_properties(zco.integration.hook_integration_test PROPERTIES
            RUN_SERIAL TRUE
            RESOURCE_LOCK zco_runtime
        )
    endif()
endif()

if(ZLYNX_BUILD_PERF_TESTS)
    # perf 目标在 RelWithDebInfo 下构建，保留符号和足够优化。
    foreach(perf_src ${ZCO_PERF_SOURCES})
        get_filename_component(perf_name ${perf_src} NAME_WE)
        zlynx_add_perf_target(${perf_name} ${perf_src} ${zco_test_runtime})
        target_include_directories(${perf_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()
endif()

if(BUILD_TESTING)
    zlynx_add_module_test_targets(zco)
endif()
