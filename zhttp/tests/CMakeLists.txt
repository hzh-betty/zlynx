find_package(GTest REQUIRED)

# 单元测试列表
set(UNIT_TESTS
    http_parser_test
    http_request_test
    http_response_test
    router_test
    middleware_test
    http_parser_detailed_test
    router_detailed_test
    regex_prefix_bucket_test
    router_comprehensive_test
    router_comprehensive_test2
)

# 为每个单元测试创建可执行文件
foreach(test_name ${UNIT_TESTS})
    add_executable(${test_name} unit/${test_name}.cc)
    target_link_libraries(${test_name}
        zhttp
        znet_static
        zcoroutine_static
        zlog_static
        GTest::gtest
        pthread
    )
    target_include_directories(${test_name} PRIVATE
        ${CMAKE_SOURCE_DIR}/zhttp/include
    )
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# 集成测试
add_executable(http_server_test integration/http_server_test.cc)
target_link_libraries(http_server_test
    zhttp
    znet_static
    zcoroutine_static
    zlog_static
    GTest::gtest
    pthread
)
target_include_directories(http_server_test PRIVATE
    ${CMAKE_SOURCE_DIR}/zhttp/include
)
add_test(NAME http_server_test COMMAND http_server_test)
