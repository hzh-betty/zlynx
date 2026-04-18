find_package(GTest REQUIRED)
find_package(Threads REQUIRED)

function(add_znet_unit_test target_name src_file)
  add_executable(${target_name} ${src_file})
  target_link_libraries(${target_name}
    PRIVATE
      znet
      zco
      GTest::gtest
      GTest::gtest_main
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
  set_tests_properties(${target_name} PROPERTIES LABELS "znet;quick" TIMEOUT 90)
endfunction()

file(GLOB ZNET_UNIT_TESTS CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/*_unit.cc"
)

foreach(test_src ${ZNET_UNIT_TESTS})
  get_filename_component(test_name ${test_src} NAME_WE)
  add_znet_unit_test(${test_name} ${test_src})
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
