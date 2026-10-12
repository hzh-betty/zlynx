if(BUILD_TESTING)
    if(NOT TARGET GTest::gtest_main)
        find_package(GTest CONFIG REQUIRED)
    endif()

    set(test_sources
        integration/zlog_integration_test.cc
        unit/buffer_test.cc
        unit/format_test.cc
        unit/level_test.cc
        unit/logger_test.cc
        unit/looper_test.cc
        unit/ownership_test.cc
        unit/sink_test.cc
        unit/util_test.cc
    )
    foreach(source IN LISTS test_sources)
        get_filename_component(name ${source} NAME_WE)
        add_executable(zlog_${name} ${source})
        target_compile_features(zlog_${name} PRIVATE cxx_std_17)
        set_target_properties(zlog_${name} PROPERTIES CXX_EXTENSIONS OFF)
        target_link_libraries(zlog_${name} PRIVATE zlog::zlog GTest::gtest_main Threads::Threads)
        target_include_directories(zlog_${name} PRIVATE ../src)
        add_test(NAME zlog_${name} COMMAND zlog_${name})
        set_tests_properties(zlog_${name} PROPERTIES TIMEOUT 90)
    endforeach()

    target_link_libraries(zlog_logger_test PRIVATE GTest::gmock)
    target_link_libraries(zlog_format_test PRIVATE GTest::gmock)
    target_link_libraries(zlog_sink_test PRIVATE GTest::gmock)
    target_link_libraries(zlog_zlog_integration_test PRIVATE GTest::gmock)
endif()

if(BUILD_BENCHMARKS)
    add_executable(zlog_performance benchmark/zlog_performance.cc)
    target_link_libraries(zlog_performance PRIVATE zlog::zlog Threads::Threads)
    find_package(spdlog CONFIG QUIET)
    if(spdlog_FOUND)
        add_executable(zlog_benchmark benchmark/zlog_benchmark.cc)
        target_link_libraries(zlog_benchmark PRIVATE zlog::zlog spdlog::spdlog Threads::Threads)
    else()
        message(STATUS "zlog_benchmark requires spdlog; zlog_performance remains available")
    endif()
endif()
