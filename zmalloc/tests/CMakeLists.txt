if(BUILD_TESTING)
    if(NOT TARGET GTest::gtest_main)
        find_package(GTest CONFIG REQUIRED)
    endif()

    set(test_sources
        integration/concurrent_alloc_test.cc
        unit/allocator_override_test.cc
        unit/central_cache_test.cc
        unit/free_list_test.cc
        unit/memory_test.cc
        unit/object_pool_test.cc
        unit/override_whitebox_test.cc
        unit/page_cache_test.cc
        unit/page_map_test.cc
        unit/size_class_test.cc
        unit/span_list_test.cc
        unit/system_alloc_test.cc
        unit/thread_cache_test.cc
        unit/transfer_cache_test.cc
        unit/zmalloc_test.cc
    )
    foreach(source IN LISTS test_sources)
        get_filename_component(name ${source} NAME_WE)
        add_executable(zmalloc_${name} ${source})
        target_compile_features(zmalloc_${name} PRIVATE cxx_std_17)
        set_target_properties(zmalloc_${name} PROPERTIES CXX_EXTENSIONS OFF)
        if(name STREQUAL "allocator_override_test")
            target_link_libraries(zmalloc_${name} PRIVATE zmalloc::override GTest::gtest_main Threads::Threads)
        else()
            target_link_libraries(zmalloc_${name} PRIVATE zmalloc::zmalloc GTest::gtest_main Threads::Threads)
        endif()
        add_test(NAME zmalloc_${name} COMMAND zmalloc_${name})
        set_tests_properties(zmalloc_${name} PROPERTIES TIMEOUT 90)
    endforeach()

    add_executable(zmalloc_cold_start_test integration/cold_start_test.cc)
    target_link_libraries(zmalloc_cold_start_test PRIVATE zmalloc::zmalloc Threads::Threads)
    add_test(NAME zmalloc_cold_start_test COMMAND zmalloc_cold_start_test)
    set_tests_properties(zmalloc_cold_start_test PROPERTIES TIMEOUT 30)

    # 白盒测试包含 override.cc，因此也需要链接其运行时依赖。
    target_link_libraries(zmalloc_override_whitebox_test PRIVATE ${CMAKE_DL_LIBS})
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(zmalloc_override_whitebox_test PRIVATE -fno-builtin -ftls-model=initial-exec)
        target_compile_options(zmalloc_allocator_override_test PRIVATE -fno-builtin)
    endif()

    if(BUILD_SHARED_LIBS)
        add_library(zmalloc_override_shared_test SHARED ../src/override.cc)
        target_link_libraries(zmalloc_override_shared_test PRIVATE zmalloc::zmalloc ${CMAKE_DL_LIBS})
        target_compile_options(zmalloc_override_shared_test PRIVATE $<TARGET_PROPERTY:zmalloc_override,COMPILE_OPTIONS>)
        set_target_properties(zmalloc_override_shared_test PROPERTIES CXX_EXTENSIONS OFF)
        add_executable(zmalloc_allocator_override_shared_test unit/allocator_override_test.cc)
        target_link_libraries(zmalloc_allocator_override_shared_test PRIVATE
            zmalloc_override_shared_test zmalloc::zmalloc GTest::gtest_main Threads::Threads)
        target_compile_options(zmalloc_allocator_override_shared_test PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang>:-fno-builtin>)
        add_test(NAME zmalloc_allocator_override_shared_test COMMAND zmalloc_allocator_override_shared_test)
        set_tests_properties(zmalloc_allocator_override_shared_test PROPERTIES TIMEOUT 90)
    endif()
endif()

if(BUILD_BENCHMARKS)
    add_executable(thread_cache_benchmark benchmark/thread_cache_benchmark.cc)
    target_link_libraries(thread_cache_benchmark PRIVATE zmalloc::zmalloc Threads::Threads)
    add_executable(zmalloc_benchmark benchmark/zmalloc_benchmark.cc)
    target_link_libraries(zmalloc_benchmark PRIVATE zmalloc::zmalloc Threads::Threads)
    add_executable(zmalloc_performance benchmark/zmalloc_performance.cc)
    target_link_libraries(zmalloc_performance PRIVATE zmalloc::zmalloc Threads::Threads)
endif()
