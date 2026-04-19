cmake_minimum_required(VERSION 3.18)

find_package(GTest REQUIRED)
find_package(Threads REQUIRED)

function(add_zco_test target_name src_file labels timeout_seconds)
  add_executable(${target_name} ${src_file})
  if(TARGET zco_stdalloc)
    set(zco_test_runtime zco_stdalloc)
  else()
    set(zco_test_runtime zco)
  endif()
  target_link_libraries(${target_name} PRIVATE ${zco_test_runtime} GTest::gtest GTest::gtest_main Threads::Threads)
  target_include_directories(${target_name} PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests)
  if(ENABLE_COVERAGE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
      target_compile_options(${target_name} PRIVATE -O0 -g --coverage)
      target_link_options(${target_name} PRIVATE --coverage)
    endif()
  endif()
  add_test(NAME ${target_name} COMMAND ${target_name})
  set_tests_properties(${target_name} PROPERTIES LABELS "${labels}" TIMEOUT ${timeout_seconds})
endfunction()

function(add_zco_benchmark target_name src_file labels timeout_seconds)
  add_executable(${target_name} ${src_file})
  if(TARGET zco_stdalloc)
    set(zco_test_runtime zco_stdalloc)
  else()
    set(zco_test_runtime zco)
  endif()
  target_link_libraries(${target_name} PRIVATE ${zco_test_runtime} Threads::Threads)
  target_include_directories(${target_name} PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests)
  if(ENABLE_COVERAGE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
      target_compile_options(${target_name} PRIVATE -O0 -g --coverage)
      target_link_options(${target_name} PRIVATE --coverage)
    endif()
  endif()
  add_test(
    NAME ${target_name}_smoke
    COMMAND ${CMAKE_COMMAND} -E env
            ZCO_PERF_SCALE_PCT=1
            $<TARGET_FILE:${target_name}>
  )
  set_tests_properties(${target_name}_smoke PROPERTIES LABELS "${labels}" TIMEOUT ${timeout_seconds})
endfunction()

file(GLOB ZCO_UNIT_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/unit/*.cc")
file(GLOB ZCO_INTEGRATION_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/integration/*.cc")
file(GLOB ZCO_STRESS_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/stress/*.cc")
set(ZCO_BENCHMARK_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/stress/stack_model_perf.cc"
)
list(REMOVE_ITEM ZCO_STRESS_TESTS ${ZCO_BENCHMARK_SOURCES})

foreach(test_src ${ZCO_UNIT_TESTS})
  get_filename_component(test_name ${test_src} NAME_WE)
  add_zco_test(${test_name} ${test_src} "unit;quick" 20)
endforeach()

foreach(test_src ${ZCO_INTEGRATION_TESTS})
  get_filename_component(test_name ${test_src} NAME_WE)
  add_zco_test(${test_name} ${test_src} "integration;quick" 45)
endforeach()

# hook_integration occasionally flakes under high global test parallelism due
# transient networking/scheduler resource pressure; run it in isolation.
if(TARGET hook_integration)
  set_tests_properties(hook_integration PROPERTIES
    RUN_SERIAL TRUE
    RESOURCE_LOCK zco_runtime
  )
endif()

foreach(test_src ${ZCO_STRESS_TESTS})
  get_filename_component(test_name ${test_src} NAME_WE)
  set(stress_labels "stress;extended")
  if(test_name MATCHES "(perf|benchmark)")
    set(stress_labels "stress;extended;performance")
  endif()
  add_zco_test(${test_name} ${test_src} "${stress_labels}" 180)
endforeach()

foreach(benchmark_src ${ZCO_BENCHMARK_SOURCES})
  get_filename_component(benchmark_name ${benchmark_src} NAME_WE)
  add_zco_benchmark(${benchmark_name} ${benchmark_src} "stress;extended;performance" 180)
endforeach()

add_custom_target(zco_test_quick
  COMMAND ${CMAKE_CTEST_COMMAND} -L quick --output-on-failure
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  COMMENT "Run zco quick tests (unit + integration)"
)

add_custom_target(zco_test_extended
  COMMAND ${CMAKE_CTEST_COMMAND} -L extended --output-on-failure
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  COMMENT "Run zco extended tests (stress)"
)

add_custom_target(zco_test_performance
  COMMAND ${CMAKE_CTEST_COMMAND} -L performance --output-on-failure
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  COMMENT "Run zco performance tests"
)
