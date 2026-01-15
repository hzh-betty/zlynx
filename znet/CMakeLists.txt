cmake_minimum_required(VERSION 3.18)

project(znet LANGUAGES CXX)

# 继承父项目的C++标准
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 查找依赖
find_package(Threads REQUIRED)

# 收集源文件
file(GLOB ZNET_SOURCES
    ${PROJECT_SOURCE_DIR}/src/*.cc
)

# 收集头文件
file(GLOB ZNET_HEADERS
    ${PROJECT_SOURCE_DIR}/include/*.h
)

# 创建共享库和静态库
add_library(znet_shared SHARED ${ZNET_SOURCES})
add_library(znet_static STATIC ${ZNET_SOURCES})

# 设置库的输出名称（去掉_shared/_static后缀）
set_target_properties(znet_shared PROPERTIES OUTPUT_NAME znet)
set_target_properties(znet_static PROPERTIES OUTPUT_NAME znet)

# 公共头文件目录
target_include_directories(znet_shared
    PUBLIC ${PROJECT_SOURCE_DIR}/include
    PUBLIC ${CMAKE_SOURCE_DIR}/zcoroutine/include
    PUBLIC ${CMAKE_SOURCE_DIR}/zlog/include
)

target_include_directories(znet_static
    PUBLIC ${PROJECT_SOURCE_DIR}/include
    PUBLIC ${CMAKE_SOURCE_DIR}/zcoroutine/include
    PUBLIC ${CMAKE_SOURCE_DIR}/zlog/include
)

# 链接依赖库
target_link_libraries(znet_shared
    PUBLIC zcoroutine_shared
    PUBLIC zlog_shared
    PUBLIC Threads::Threads
    PUBLIC dl
)

target_link_libraries(znet_static
    PUBLIC zcoroutine_static
    PUBLIC zlog_static
    PUBLIC Threads::Threads
    PUBLIC dl
)

# 安装规则（可选）
install(TARGETS znet_shared znet_static
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(FILES ${ZNET_HEADERS}
    DESTINATION include/znet
)

message(STATUS "==============================")
message(STATUS "znet configuration")
message(STATUS "------------------------------")
message(STATUS "C++ Standard      : ${CMAKE_CXX_STANDARD}")
message(STATUS "Source files      : ${ZNET_SOURCES}")
message(STATUS "Shared Library    : znet_shared")
message(STATUS "Static Library    : znet_static")
message(STATUS "==============================")

# 添加测试子目录
add_subdirectory(tests)
