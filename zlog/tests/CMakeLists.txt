cmake_minimum_required(VERSION 3.18)

find_package(GTest REQUIRED)
find_package(Threads REQUIRED)
find_package(spdlog QUIET)
find_package(glog QUIET)

# BUILD_TESTING 只控制 unit/integration 的 CTest 注册。
# benchmark/performance 由 ZLYNX_BUILD_PERF_TESTS 单独控制，避免默认测试跑性能用例。
if(BUILD_TESTING)
    # zlog 测试文件按目录分类；CONFIGURE_DEPENDS 让新增测试文件时自动触发重新配置。
    file(GLOB ZLOG_UNIT_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/unit/*.cc")
    file(GLOB ZLOG_INTEGRATION_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/integration/*.cc")

    foreach(test_src ${ZLOG_UNIT_TESTS})
        get_filename_component(test_name ${test_src} NAME_WE)
        zlynx_add_gtest(zlog_${test_name} ${test_src} zlog unit zlog)
    endforeach()

    foreach(test_src ${ZLOG_INTEGRATION_TESTS})
        get_filename_component(test_name ${test_src} NAME_WE)
        zlynx_add_gtest(zlog_${test_name} ${test_src} zlog integration zlog)
    endforeach()
endif()

if(ZLYNX_BUILD_PERF_TESTS)
    # 对比 benchmark 依赖第三方日志库，缺依赖时跳过该目标，但保留自有 performance 目标。
    if(spdlog_FOUND AND glog_FOUND)
        add_executable(zlog_benchmark benchmark/zlog_benchmark.cc)
        target_link_libraries(zlog_benchmark PRIVATE zlog spdlog::spdlog glog::glog Threads::Threads)
        zlynx_apply_perf_options(zlog_benchmark)
    else()
        message(STATUS "zlog benchmark target disabled: spdlog or glog was not found")
    endif()

    zlynx_add_perf_target(zlog_performance benchmark/zlog_performance.cc zlog)
endif()
