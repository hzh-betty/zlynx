if(BUILD_TESTING)
    if(NOT TARGET GTest::gtest_main)
        find_package(GTest CONFIG REQUIRED)
    endif()

    # --wrap 无法拦截已经在共享库内部完成链接的调用。
    if(BUILD_SHARED_LIBS)
        get_target_property(zco_sources zco SOURCES)
        get_target_property(zco_standard zco CXX_STANDARD)
        list(TRANSFORM zco_sources PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/../")
        add_library(zco_test_static STATIC ${zco_sources})
        target_compile_features(zco_test_static PUBLIC $<TARGET_PROPERTY:zco,INTERFACE_COMPILE_FEATURES>)
        target_compile_options(zco_test_static PRIVATE $<TARGET_PROPERTY:zco,COMPILE_OPTIONS>)
        target_include_directories(zco_test_static PUBLIC ../include PRIVATE ../src)
        target_link_libraries(zco_test_static PUBLIC Threads::Threads)
        set_target_properties(zco_test_static PROPERTIES
            CXX_STANDARD ${zco_standard} CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    endif()

    set(test_sources
        integration/descriptor_lifecycle_test.cc
        integration/runtime_shutdown_test.cc
        integration/socket_io_test.cc
        unit/descriptor_flags_test.cc
        unit/io_operations_test.cc
        unit/io_wait_registry_test.cc
        unit/runtime_lifecycle_test.cc
        unit/stack_arena_test.cc
        unit/synchronization_test.cc
        unit/task_completion_test.cc
        unit/task_scheduling_test.cc
        unit/timer_queue_test.cc
        unit/wait_queue_test.cc
        unit/wait_state_test.cc
    )
    foreach(source IN LISTS test_sources)
        get_filename_component(name ${source} NAME_WE)
        add_executable(zco_${name} ${source})
        target_compile_features(zco_${name} PRIVATE cxx_std_17)
        set_target_properties(zco_${name} PROPERTIES CXX_EXTENSIONS OFF)
        if(BUILD_SHARED_LIBS AND name MATCHES "^(runtime_lifecycle|task_completion|descriptor_flags|task_scheduling)_test$")
            target_link_libraries(zco_${name} PRIVATE zco_test_static GTest::gtest_main Threads::Threads)
        else()
            target_link_libraries(zco_${name} PRIVATE zco::zco GTest::gtest_main Threads::Threads)
        endif()
        target_include_directories(zco_${name} PRIVATE ../src ${CMAKE_CURRENT_SOURCE_DIR})
        add_test(NAME zco_${name} COMMAND zco_${name})
        set_tests_properties(zco_${name} PROPERTIES TIMEOUT 90)
    endforeach()

    target_link_libraries(zco_runtime_lifecycle_test PRIVATE ${CMAKE_DL_LIBS})
    target_link_options(zco_runtime_lifecycle_test PRIVATE LINKER:--wrap=epoll_create1)
    target_link_options(zco_task_completion_test PRIVATE LINKER:--wrap=_Znwm)
    target_link_options(zco_descriptor_flags_test PRIVATE LINKER:--wrap=fcntl)
    target_link_options(zco_task_scheduling_test PRIVATE
        LINKER:--wrap=swapcontext LINKER:--wrap=getcontext LINKER:--wrap=_Znwm)
endif()

if(BUILD_BENCHMARKS)
    add_executable(zco_performance benchmark/zco_performance.cc)
    target_link_libraries(zco_performance PRIVATE zco::zco Threads::Threads)
    target_include_directories(zco_performance PRIVATE ../src)
    target_compile_definitions(zco_performance PRIVATE ZCO_BENCHMARK_METRICS)
endif()
