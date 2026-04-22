find_package(GTest REQUIRED)
find_package(Threads REQUIRED)

function(add_znet_test target_name src_file labels)
  add_executable(${target_name} ${src_file})
  target_link_libraries(${target_name}
    PRIVATE
      znet
      zco
      GTest::gtest
      Threads::Threads
  )
  target_include_directories(${target_name}
    PRIVATE
      ${PROJECT_SOURCE_DIR}/include
  )
  if(ENABLE_COVERAGE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
      target_compile_options(${target_name} PRIVATE -O0 -g --coverage)
      target_link_options(${target_name} PRIVATE --coverage)
    endif()
  endif()
  add_test(NAME ${target_name} COMMAND ${target_name})
  set_tests_properties(${target_name} PROPERTIES LABELS "${labels}" TIMEOUT 90)
endfunction()

file(GLOB ZNET_UNIT_TESTS CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/unit/*.cc"
)

file(GLOB ZNET_INTEGRATION_TESTS CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/integration/*.cc"
)

foreach(test_src ${ZNET_UNIT_TESTS})
  get_filename_component(test_name ${test_src} NAME_WE)
  add_znet_test(${test_name} ${test_src} "znet;unit;quick")
endforeach()

foreach(test_src ${ZNET_INTEGRATION_TESTS})
  get_filename_component(test_name ${test_src} NAME_WE)
  add_znet_test(${test_name} ${test_src} "znet;integration;quick")
endforeach()

# These tests are sensitive to system-level socket/thread pressure when many
# binaries run concurrently. Serialize them to reduce flaky crashes.
set(ZNET_SERIAL_TESTS
  buffer_test
  server_test
  tcp_server_test
)
foreach(test_name ${ZNET_SERIAL_TESTS})
  if(TARGET ${test_name})
    set_tests_properties(${test_name} PROPERTIES
      RUN_SERIAL TRUE
      RESOURCE_LOCK zco_runtime
    )
  endif()
endforeach()

add_executable(znet_wrk_benchmark
  benchmark/znet_wrk_benchmark.cc
)

target_link_libraries(znet_wrk_benchmark
  PRIVATE
    znet
    zco
    Threads::Threads
)

target_include_directories(znet_wrk_benchmark
  PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)

if(ENABLE_COVERAGE)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(znet_wrk_benchmark PRIVATE -O0 -g --coverage)
    target_link_options(znet_wrk_benchmark PRIVATE --coverage)
  endif()
endif()
