cmake_minimum_required(VERSION 3.18)

project(zmalloc LANGUAGES CXX)

# 使用 C++11 标准
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 查找依赖
find_package(Threads REQUIRED)

# 收集源文件
set(ZMALLOC_SOURCES
    src/size_class.cc
    src/span.cc
    src/page_heap.cc
    src/central_cache.cc
    src/thread_cache.cc
    src/zmalloc.cc
)

# 收集头文件
file(GLOB ZMALLOC_HEADERS
    ${PROJECT_SOURCE_DIR}/include/*.h
)

# 创建共享库和静态库
add_library(zmalloc_shared SHARED ${ZMALLOC_SOURCES})
add_library(zmalloc_static STATIC ${ZMALLOC_SOURCES})

# 设置库的输出名称（去掉_shared/_static后缀）
set_target_properties(zmalloc_shared PROPERTIES OUTPUT_NAME zmalloc)
set_target_properties(zmalloc_static PROPERTIES OUTPUT_NAME zmalloc)

# 公共头文件目录
target_include_directories(zmalloc_shared
    PUBLIC ${PROJECT_SOURCE_DIR}/include
)

target_include_directories(zmalloc_static
    PUBLIC ${PROJECT_SOURCE_DIR}/include
)

# 链接依赖库
target_link_libraries(zmalloc_shared
    PUBLIC Threads::Threads
)

target_link_libraries(zmalloc_static
    PUBLIC Threads::Threads
)

# 安装规则
install(TARGETS zmalloc_shared zmalloc_static
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(FILES ${ZMALLOC_HEADERS}
    DESTINATION include/zmalloc
)

# 测试选项
option(BUILD_ZMALLOC_TESTS "Build zmalloc tests" ON)

if(BUILD_ZMALLOC_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "==============================")
message(STATUS "zmalloc configuration")
message(STATUS "------------------------------")
message(STATUS "C++ Standard      : ${CMAKE_CXX_STANDARD}")
message(STATUS "Shared Library    : zmalloc_shared")
message(STATUS "Static Library    : zmalloc_static")
message(STATUS "Tests enabled     : ${BUILD_ZMALLOC_TESTS}")
message(STATUS "==============================")
