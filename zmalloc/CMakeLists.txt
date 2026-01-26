cmake_minimum_required(VERSION 3.18)

project(zmalloc VERSION 1.0.0 LANGUAGES CXX)

# C++11 标准
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 查找线程库
find_package(Threads REQUIRED)

# 源文件
set(ZMALLOC_SOURCES
    src/size_class.cc
    src/size_class_lookup.cc
    src/system_alloc.cc
    src/transfer_cache.cc
    src/thread_cache.cc
    src/central_cache.cc
    src/page_cache.cc
    src/span_list.cc
)

# 头文件目录
set(ZMALLOC_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)

# 构建库
add_library(zmalloc ${ZMALLOC_SOURCES})

target_include_directories(zmalloc
    PUBLIC
        $<BUILD_INTERFACE:${ZMALLOC_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(zmalloc
    PUBLIC
        Threads::Threads
)

# 编译选项
target_compile_options(zmalloc PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

# 测试子目录
if(ENABLE_TESTS)
    add_subdirectory(tests)
endif()

# 导出
set_target_properties(zmalloc PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
)
